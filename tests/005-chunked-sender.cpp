#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <quic.hpp>
#include <quic/gnutls_crypto.hpp>
#include <thread>

namespace oxen::quic::test
{
    using namespace std::literals;

    TEST_CASE("005: Chunked stream sending", "[005][chunked]")
    {
        logger_config();

        Network test_net{};

        std::mutex recv_mut;
        std::string received;
        stream_data_callback_t stream_data_cb = [&](Stream&, bstring_view data) {
            std::lock_guard lock{recv_mut};
            received.append(reinterpret_cast<const char*>(data.data()), data.size());
        };

        auto server_tls = GNUTLSCreds::make("./serverkey.pem"s, "./servercert.pem"s, "./clientcert.pem"s);
        auto client_tls = GNUTLSCreds::make("./clientkey.pem"s, "./clientcert.pem"s, "./servercert.pem"s);

        opt::local_addr server_local{"127.0.0.1"s, 5500};
        opt::local_addr client_local{"127.0.0.1"s, 4400};
        opt::remote_addr client_remote{"127.0.0.1"s, 5500};

        auto server_endpoint = test_net.endpoint(server_local);
        server_endpoint->listen(server_tls, stream_data_cb);

        auto client_endpoint = test_net.endpoint(client_local);
        auto conn_interface = client_endpoint->connect(client_remote, client_tls);

        auto stream = conn_interface->get_new_stream();
        stream->send("HELLO!"s);

        int i = 0;
        constexpr size_t parallel_chunks = 2;
        std::array<std::vector<char>, parallel_chunks> bufs;

        stream->send_chunks(
                [&](const Stream& s) {
                    log::critical(log_cat, "getting next chunk ({}) for stream {}", i, s.stream_id);
                    if (i++ < 3)
                        return fmt::format("[CHUNK-{}]", i);
                    i--;
                    return ""s;
                },
                [&](Stream& s) {
                    auto pointer_chunks = [&](const Stream& s) -> std::vector<char>* {
                        log::critical(log_cat, "getting next chunk ({}) for stream {}", i, s.stream_id);
                        if (i++ < 6)
                        {
                            auto& vec = bufs[i % parallel_chunks];
                            vec.clear();
                            fmt::format_to(std::back_inserter(vec), "[Chunk-{}]", i);
                            return &vec;
                        }
                        i--;
                        return nullptr;
                    };

                    s.send_chunks(
                            pointer_chunks,
                            [&](Stream& s) {
                                auto smart_ptr_chunks = [&](const Stream& s) -> std::unique_ptr<std::vector<char>> {
                                    log::critical(log_cat, "getting next chunk ({}) for stream {}", i, s.stream_id);
                                    if (i++ >= 10)
                                        return nullptr;
                                    auto vec = std::make_unique<std::vector<char>>();
                                    fmt::format_to(std::back_inserter(*vec), "[chunk-{}]", i);
                                    return vec;
                                };
                                s.send_chunks(
                                        smart_ptr_chunks,
                                        [&](Stream& s) {
                                            // (Lokinet RPC was here)
                                            log::critical(log_cat, "All chunks done!");
                                            s.send("Goodbye."s);
                                        },
                                        parallel_chunks);
                            },
                            parallel_chunks);
                },
                parallel_chunks);

        std::this_thread::sleep_for(100ms);

        {
            std::lock_guard lock{recv_mut};
            CHECK(received ==
                  "HELLO![CHUNK-1][CHUNK-2][CHUNK-3][Chunk-4][Chunk-5][Chunk-6][chunk-7][chunk-8][chunk-9][chunk-10]"
                  "Goodbye.");
        }

        test_net.close();
    };
}  // namespace oxen::quic::test
