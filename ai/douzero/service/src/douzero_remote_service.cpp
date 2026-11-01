#include "mygames/douzero_remote_service.hpp"
#include "mygames/douzero_remote_wire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace mygames::douzero {

namespace {
constexpr unsigned kHttpUnauthorized = 403u;
constexpr unsigned kHttpInvalid = 400u;
constexpr unsigned kHttpUnsupported = 421u;
constexpr unsigned kHttpBusy = 429u;
constexpr unsigned kHttpModelFailure = 503u;
constexpr unsigned kHttpOk = 200u;

int reject(chttp_server_response* reply, unsigned status) {
    return chttp_server_reply(reply, status, "text/plain",
                              nullptr, 0u);
}
bool equal_cert(const char* remote, const std::string& trusted) {
    if (!remote || trusted.size() != 64u || std::strlen(remote) != 64u)
        return false;
    unsigned diff = 0u;
    for (std::size_t i = 0; i < 64u; ++i)
        diff |= static_cast<unsigned>(
            static_cast<unsigned char>(remote[i]) ^
            static_cast<unsigned char>(trusted[i]));
    return diff == 0u;
}
} // namespace

bool RemoteInferenceService::valid_sha_hex(
    const std::string& value) noexcept {
    if (value.size() != 64u) return false;
    for (const char c : value)
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
            return false;
    return true;
}
RemoteInferenceService::RemoteInferenceService(Config config)
    : config_(std::move(config)) {
    if (!config_.model_version || config_.worker_count < 1u ||
        config_.worker_count > 16u || config_.max_inflight < 1u ||
        config_.max_inflight > 1024u ||
        !valid_sha_hex(config_.authorized_client_certificate_sha256))
        throw std::invalid_argument("invalid original-DouZero inference service admission");
    for (std::size_t i = 0; i < 3u; ++i) {
        if (config_.role_models[i] == nullptr)
            throw std::invalid_argument("all three original DouZero Q role packages required");
        std::uint8_t bits = 0;
        for (const std::uint8_t x : config_.model_artifact_sha256[i]) bits |= x;
        if (!bits) throw std::invalid_argument("exact ONNX artifact SHA must be pinned");
    }
    try {
        workers_.reserve(config_.worker_count);
        for (std::size_t i = 0; i < config_.worker_count; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        for (auto& thread : workers_) if (thread.joinable()) thread.join();
        throw;
    }
}
RemoteInferenceService::~RemoteInferenceService() {
    /* IMPORTANT: the owning host must ALREADY have stopped and destroyed the
     * separate CHttp server before releasing its callback context. Existing
     * outstanding deferred H2 response slots are drained by chttp_server_stop.
     */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
}

bool RemoteInferenceService::admit_peer(
    const chttp_server_request_view& request) const noexcept {
    /* This value exists only after successful CHttp/CNet TLS certificate
     * verification. No plaintext, no JWT fallback, no unauthenticated WS. */
    return equal_cert(request.peer_certificate_sha256,
                      config_.authorized_client_certificate_sha256);
}
bool RemoteInferenceService::admit_frame(
    const void* bytes, std::size_t n) const noexcept {
    if (bytes == nullptr || n > MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX)
        return false;
    mygames_douzero_request_view view{};
    if (mygames_douzero_wire_decode_request(bytes, n, &view) !=
        MYGAMES_DOUZERO_WIRE_OK)
        return false;
    const std::size_t role = view.ticket.role;
    return view.ticket.model_version == config_.model_version &&
           config_.role_models[role] != nullptr &&
           std::memcmp(view.ticket.model_artifact_sha256,
                       config_.model_artifact_sha256[role].data(), 32u) == 0;
}
bool RemoteInferenceService::mount(chttp_server* server) {
    if (server == nullptr || mounted_) return false;
    /* Route registration does not enable insecure HTTP; host MUST bind mTLS.
     * H1 POST is explicitly rejected in on_http. WSS may be H1 Upgrade or
     * H2 Extended CONNECT on the same independent authenticated listener. */
    mounted_ = true;
    if (chttp_server_post(server, kPath, &on_http, this) != SALTS_OK)
        return false;
    chttp_server_websocket_options ws{};
    ws.size = sizeof(ws);
    ws.path = kPath;
    ws.max_frame_bytes = MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX;
    ws.max_message_bytes = MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX;
    ws.max_buffered_input_bytes = MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX + 14u;
    ws.on_open = &on_ws_open;
    ws.on_event = &on_ws_event;
    ws.user = this;
    return chttp_server_websocket_with(server, &ws) == SALTS_OK;
}

bool RemoteInferenceService::queue_work(Work&& work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || inflight_ >= config_.max_inflight) return false;
    try {
        queue_.push_back(std::move(work));
    } catch (...) {
        return false;
    }
    ++inflight_;
    changed_.notify_one();
    return true;
}

int RemoteInferenceService::on_http(
    void* user, const chttp_server_request_view* request,
    chttp_server_response* response) {
    auto* self = static_cast<RemoteInferenceService*>(user);
    if (!self || !request || !response) return SALTS_EINVAL;
    if (!self->admit_peer(*request))
        return reject(response, kHttpUnauthorized);
    if (request->http_major != 2u)
        return reject(response, kHttpUnsupported);
    const char* type = chttp_server_request_header(request, "content-type");
    if (!type || std::strcmp(type, kContentType) != 0 ||
        request->body_streamed || !self->admit_frame(
            request->body, request->body_size))
        return reject(response, kHttpInvalid);

    Work work;
    try {
        work.request.assign(
            static_cast<const std::uint8_t*>(request->body),
            static_cast<const std::uint8_t*>(request->body) + request->body_size);
    } catch (...) {
        return reject(response, kHttpModelFailure);
    }
    /* Defer rather than run potentially slow ONNX computation on fixed CHttp
     * connection/stream owner. H2 stream generation controls late replies. */
    if (chttp_server_response_defer(response, &work.deferred) != SALTS_OK)
        return SALTS_EIO;
    if (!self->queue_work(std::move(work))) {
        /* Once deferred it cannot become a synchronous 429. Cancel the
         * stream, never retry or execute a second model, and let the client
         * close this generation-specific AI proposal on timeout. */
        (void)chttp_server_deferred_cancel(&work.deferred);
    }
    return SALTS_OK;
}

int RemoteInferenceService::on_ws_open(
    void* user, chttp_websocket* peer,
    const chttp_server_request_view* request,
    chttp_server_response* response) {
    auto* self = static_cast<RemoteInferenceService*>(user);
    (void)peer;
    if (!self || !request || !response) return SALTS_EINVAL;
    if (!self->admit_peer(*request))
        return reject(response, kHttpUnauthorized);
    /* Must explicitly negotiate the original binary model subprotocol,
     * rather than accepting arbitrary WSS clients with no contract. */
    if (chttp_server_response_select_websocket_subprotocol(
            response, request, kSubprotocol) != SALTS_OK)
        return reject(response, kHttpInvalid);
    return SALTS_OK;
}

void RemoteInferenceService::on_ws_event(
    void* user, chttp_websocket* peer, const chttp_websocket_event* event) {
    auto* self = static_cast<RemoteInferenceService*>(user);
    if (!self || !peer || !event ||
        event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE) return;
    if (event->message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY ||
        !self->admit_frame(event->data, event->size)) {
        (void)chttp_websocket_close(peer, 1003u, nullptr, 0u);
        return;
    }
    Work work;
    work.websocket = true;
    if (chttp_server_websocket_session_capture(peer, &work.session) != SALTS_OK) {
        (void)chttp_websocket_close(peer, 1011u, nullptr, 0u);
        return;
    }
    try {
        work.request.assign(event->data, event->data + event->size);
    } catch (...) {
        (void)chttp_websocket_close(peer, 1011u, nullptr, 0u);
        return;
    }
    if (!self->queue_work(std::move(work))) {
        (void)chttp_websocket_close(peer, 1013u, nullptr, 0u);
    }
}

void RemoteInferenceService::worker_loop() noexcept {
    for (;;) {
        Work work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) return;
                continue;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        process(work);
        std::fill(work.request.begin(), work.request.end(), std::uint8_t{0});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inflight_;
        }
    }
}

void RemoteInferenceService::process(Work& work) noexcept {
    std::array<std::uint8_t, MYGAMES_DOUZERO_REMOTE_REPLY_BYTES_MAX> reply{};
    std::array<float, MYGAMES_DOUZERO_REMOTE_BATCH_MAX> scores{};
    std::size_t reply_bytes = 0u;
    bool ok = false;
    try {
        mygames_douzero_request_view view{};
        if (mygames_douzero_wire_decode_request(
                work.request.data(), work.request.size(), &view) ==
            MYGAMES_DOUZERO_WIRE_OK) {
            const auto role = view.ticket.role;
            const std::size_t width = role == 0u ? 319u : 430u;
            ok = admit_frame(work.request.data(), work.request.size()) &&
                 kfcore_dmc_q_score(
                     config_.role_models[role], view.ticket.model_version,
                     1u, role, view.x, width, view.z, 810u,
                     view.actions, view.ticket.action_count, scores.data()) ==
                     KFCORE_DMC_Q_OK &&
                 mygames_douzero_wire_encode_reply(
                     &view.ticket, scores.data(), view.ticket.action_count,
                     reply.data(), reply.size(), &reply_bytes) ==
                     MYGAMES_DOUZERO_WIRE_OK;
        }
    } catch (...) {
        ok = false;
    }
    if (work.websocket) {
        if (ok) {
            /* CHttp copies into its bounded owner command queue; accepted
             * means submitted, not delivered. The C11 caller checks the
             * exact owner ticket and TIMES OUT when any chunk is missing. */
            (void)chttp_server_websocket_send_binary(
                &work.session, reply.data(), reply_bytes);
        } else {
            (void)chttp_server_websocket_close(
                &work.session, 1011u, nullptr, 0u);
        }
    } else {
        chttp_server_deferred_response result{};
        result.size = sizeof(result);
        result.status_code = ok ? kHttpOk : kHttpModelFailure;
        result.content_type = ok ? kContentType : "text/plain";
        result.body = ok ? reply.data() : nullptr;
        result.body_size = ok ? reply_bytes : 0u;
        if (chttp_server_deferred_reply(&work.deferred, &result) != SALTS_OK)
            (void)chttp_server_deferred_cancel(&work.deferred);
    }
    std::fill(scores.begin(), scores.end(), 0.0f);
}
} // namespace mygames::douzero
