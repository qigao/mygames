#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <http_server/http.h>
#include <kfcore/dmc_q.h>

namespace mygames::douzero {

/* C++17 inference lives in a SEPARATE service process. C11 Game Owner sees
 * only the pure-C11 wire codec and CHttp client. No Torch, C++, KFCore,
 * ONNX Runtime or inference worker belongs to the game/Room process.
 *
 * The SERVICE HOST must initialize an independent CHttp server with:
 *   - verified mTLS client certificates (not merely HTTP bearer/loopback)
 *   - HTTP/2 enabled with request and response limits >= wire maxima
 *   - fixed, bounded connection/H2 stream/WS command capacities & deadlines
 *   - trusted exact original-WP KFCore ONNX packages/role/model SHA256
 *
 * Register before CHttp start; after stopping and destroying CHttp, destroy
 * this service. Never call blocking model inference on a CHttp owner thread.
 */
class RemoteInferenceService final {
public:
    struct Config {
        std::array<kfcore_dmc_q_handle*, 3> role_models{};
        std::array<std::array<std::uint8_t, 32>, 3> model_artifact_sha256{};
        std::uint64_t model_version = 0;
        /* 64 lowercase hex digits from CHttp's verified TLS peer leaf. */
        std::string authorized_client_certificate_sha256;
        std::size_t max_inflight = 32;
        std::size_t worker_count = 2;
    };

    explicit RemoteInferenceService(Config config);
    ~RemoteInferenceService();

    RemoteInferenceService(const RemoteInferenceService&) = delete;
    RemoteInferenceService& operator=(const RemoteInferenceService&) = delete;

    /* No route/worker ownership transfer; Config KFCore handles borrowed.
     * A failed partial mount requires stopping/destroying the CHttp server
     * BEFORE destruction of this object (CHttp has no unmount operation).
     */
    bool mount(chttp_server* independently_owned_chttp_server);
    static constexpr const char* kPath = "/v1/douzero/q";
    static constexpr const char* kSubprotocol = "mygames.douzero.q.v1";
    static constexpr const char* kContentType =
        "application/vnd.mygames.douzero-q.v1";

private:
    struct Work {
        bool websocket = false;
        std::vector<std::uint8_t> request; /* own callback-borrowed bytes */
        chttp_server_deferred deferred = CHTTP_SERVER_DEFERRED_INIT;
        chttp_server_websocket_session session{};
    };

    static int on_http(void* user, const chttp_server_request_view* request,
                       chttp_server_response* response);
    static int on_ws_open(void* user, chttp_websocket* peer,
                          const chttp_server_request_view* request,
                          chttp_server_response* response);
    static void on_ws_event(void* user, chttp_websocket* peer,
                            const chttp_websocket_event* event);

    bool admit_peer(const chttp_server_request_view& request) const noexcept;
    bool admit_frame(const void* bytes, std::size_t n) const noexcept;
    bool queue_work(Work&& work);
    void worker_loop() noexcept;
    void process(Work& work) noexcept;
    static bool valid_sha_hex(const std::string& value) noexcept;

    Config config_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Work> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool mounted_ = false;
};

} // namespace mygames::douzero
