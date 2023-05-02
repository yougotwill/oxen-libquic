#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <gnutls/gnutls.h>

#include <uvw.hpp>

#include <iostream>
#include <cassert>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdexcept>
#include <sys/socket.h>
#include <filesystem>

/*
 * Example 1: Handshake with www.google.com
 *
 * #define REMOTE_HOST "www.google.com"
 * #define REMOTE_PORT "443"
 * #define ALPN "\x2h3"
 *
 * and undefine MESSAGE macro.
 */

namespace oxen::quic
{
    using namespace std::literals;
    using bstring = std::basic_string<std::byte>;

    //	Callbacks for opening quic connections and closing tunnels
    using open_callback = std::function<void(bool success, void* user_data)>;
    using close_callback = std::function<void(int rv, void* user_data)>;
	//	Callbacks for ev timer functionality
	using read_callback = std::function<void(uvw::Loop* loop, uvw::TimerEvent* ev, int revents)>;
	using timer_callback = std::function<void(int nwrite, void* user_data)>;
	//	Callback for client/server connectivity
	using server_callback = std::function<int(gnutls_session_t session, unsigned int htype,
        unsigned int when, unsigned int incoming, const gnutls_datum_t* msg)>;
    using client_callback = std::function<int(gnutls_session_t session, unsigned int htype,
        unsigned int when, unsigned int incoming, const gnutls_datum_t* msg)>;

    static constexpr std::byte CLIENT_TO_SERVER{1};
    static constexpr std::byte SERVER_TO_CLIENT{2};
    static constexpr size_t dgram_size = 1200;
    static constexpr size_t ev_loop_queue_size = 1024;

    // Max theoretical size of a UDP packet is 2^16-1 minus IP/UDP header overhead
    static constexpr size_t max_bufsize = 64 * 1024;
    // Max size of a UDP packet that we'll send
    static constexpr size_t max_pkt_size_v4 = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
    static constexpr size_t max_pkt_size_v6 = NGTCP2_MAX_UDP_PAYLOAD_SIZE;

    // Remote TCP connection was established and is now accepting stream data; the client is not allowed 
    // to send any other data down the stream until this comes back (any data sent down the stream before 
    // then is discarded)
    static constexpr std::byte CONNECT_INIT{0x00};
    // Failure to establish an initial connection:
    static constexpr uint64_t ERROR_CONNECT{0x5471907};
    // Error for something other than CONNECT_INIT as the initial stream data from the server
    static constexpr uint64_t ERROR_BAD_INIT{0x5471908};
    // Close error code sent if we get an error on the TCP socket (other than an initial connect failure)
    static constexpr uint64_t ERROR_TCP{0x5471909};
    // Application error code we close with if the data handle throws
    inline constexpr uint64_t STREAM_ERROR_EXCEPTION = (1ULL << 62) - 2;
    // Error code we send to a stream close callback if the stream's connection expires
    inline constexpr uint64_t STREAM_ERROR_CONNECTION_EXPIRED = (1ULL << 62) + 1;

    // We pause reading from the local TCP socket if we have more than this amount of outstanding
    // unacked data in the quic tunnel, then resume once it drops below this.
    inline constexpr size_t PAUSE_SIZE = 64 * 1024;

    // For templated parameter strict type checking
    template <typename Base, typename T>
    inline constexpr bool is_strict_base_of_v = std::is_base_of_v<Base, T> && !std::is_same_v<Base, T>;

    // We send and verify this in the initial connection and handshake; this is designed to allow
    // future changes (by either breaking or handling backwards compat).
    constexpr const std::array<uint8_t, 8> handshake_magic_bytes{
        'l', 'o', 'k', 'i', 'n', 'e', 't', 0x01};
    constexpr std::basic_string_view<uint8_t> handshake_magic{
        handshake_magic_bytes.data(), handshake_magic_bytes.size()};

    /*
    const char priority[] =
        "NORMAL:-VERS-ALL:+VERS-TLS1.3:-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:"
        "+CHACHA20-POLY1305:+AES-128-CCM:-GROUP-ALL:+GROUP-SECP256R1:+GROUP-X25519:"
        "+GROUP-SECP384R1:"
        "+GROUP-SECP521R1:%DISABLE_TLS13_COMPAT_MODE";
    */

    uint64_t
    get_timestamp();

    std::string
    str_tolower(std::string s);

    std::mt19937 make_mt19937();

    // Wrapper for ngtcp2_cid with helper functionalities to make it passable
    struct alignas(size_t) ConnectionID : ngtcp2_cid
    {
        ConnectionID() = default;
        ConnectionID(const ConnectionID& c) = default;
        ConnectionID(const uint8_t* cid, size_t length);
        ConnectionID(ngtcp2_cid c) : ConnectionID(c.data, c.datalen) {}
        
        ConnectionID&
        operator=(const ConnectionID& c) = default;

        inline bool
        operator==(const ConnectionID& other) const
        {
            return datalen == other.datalen && std::memcmp(data, other.data, datalen) == 0;
        }
        inline bool
        operator!=(const ConnectionID& other) const
        { 
            return !(*this == other);
        }
        static ConnectionID
        random(size_t size = NGTCP2_MAX_CIDLEN);

    };


    // Wrapper for address types with helper functionalities, operators, etc. By inheriting from
    // uvw::Addr, we are able to use string/uint16_t representations of host/port through the API
    // interface and in the constructors. The string/uint16_t representation is stored in a two
    // other formats for ease of use with ngtcp2: sockaddr_in and ngtcp2_addr. ngtcp2_addr store
    // a member of type ngtcp2_sockaddr, which is itself a typedef of sockaddr
    struct Address : public uvw::Addr
    {
        private:
            sockaddr_in _sock_addr;
            ngtcp2_addr _addr;

        public:
            std::string string_addr;

            Address() = default;
            explicit Address(std::string addr, uint16_t port);
            explicit Address(uvw::Addr addr) : Address{addr.ip, static_cast<uint16_t>(addr.port)} {};
            
            Address(const Address& obj) { *this = obj; }

            inline Address& operator=(const Address& obj)
            {
                std::memmove(&_sock_addr, &obj._sock_addr, sizeof(_sock_addr));
                _addr = obj._addr;
                _addr.addrlen = obj._addr.addrlen;
                return *this;
            }

            // can pass Address object as boolean to check if addr is set
            operator bool() const { return _sock_addr.sin_port; }
            //  template code to implicitly convert to uvw::Addr, sockaddr*, 
            //  sockaddr&, ngtcp2_addr&, and sockaddr_in&
            template <typename T, std::enable_if_t<std::is_same_v<T, sockaddr>, int> = 0>
            operator T*()
            { return reinterpret_cast<sockaddr*>(&_sock_addr); }
            template <typename T, std::enable_if_t<std::is_same_v<T, sockaddr>, int> = 0>
            operator const T*() const
            { return reinterpret_cast<const sockaddr*>(&_sock_addr); }
            inline operator
            sockaddr&() { return reinterpret_cast<sockaddr&>(_sock_addr); }
            inline operator const
            sockaddr&() const { return reinterpret_cast<const sockaddr&>(_sock_addr); }
            inline operator
            sockaddr_in&() { return _sock_addr; }
            inline operator const
            sockaddr_in&() const { return _sock_addr; }
            inline operator 
            ngtcp2_addr&() { return _addr; }
            inline operator const 
            ngtcp2_addr&() const { return _addr; }

            inline friend bool operator==(const Address& lhs, const Address& rhs)
            {
                
                if ((lhs._sock_addr.sin_addr.s_addr != rhs._sock_addr.sin_addr.s_addr) ||
                    (lhs._sock_addr.sin_port != rhs._sock_addr.sin_port) || 
                    (lhs._sock_addr.sin_family != rhs._sock_addr.sin_family) || 
                    (lhs._addr.addr->sa_data != rhs._addr.addr->sa_data))
                    return false;
                return true;
            }

            inline ngtcp2_socklen 
            sockaddr_size() const { return sizeof(sockaddr_in); }
    };

    //  Wrapper for ngtcp2_path with remote/local components. Implicitly convertible
    //  to ngtcp2_path*
    struct Path
    {
        private:
            Address _local, _remote;

        public:
            ngtcp2_path path{ 
                {_local, _local.sockaddr_size()}, {_remote, _remote.sockaddr_size()}, nullptr};

            const Address& local = _local;
            const Address& remote = _remote;
            
            Path() = default;
            Path(const Address& l, const Address& r) : _local{l}, _remote{r} {}
            Path(const Path& p) : Path{p.local, p.remote} {}

            Path& operator=(const Path& p)
            {
                _local = p._local;
                _remote = p._remote;
                return *this;
            }

            // template code to pass Path into ngtcp2 functions
            template <typename T, std::enable_if_t<std::is_same_v<T, ngtcp2_path>, int> = 0>
            operator T*() { return &path; }
            template <typename T, std::enable_if_t<std::is_same_v<T, ngtcp2_path>, int> = 0>
            operator const T*() const { return &path; }
    };

    //  Simple struct wrapping a packet and its corresponding information
    struct Packet
    {
        Path path;
        bstring data;
        ngtcp2_pkt_info pkt_info;
    };

    //  Struct returned as a result of send_packet that either is implicitly
    //  convertible to bool, but also is able to carry an error code
    struct io_result
    {
        int error_code{0};
        // returns true if this was successful
        operator bool() const { return error_code == 0; }
        //  returns true if error value indicates a failure to write
        //  without blocking
        bool blocked() const { return error_code == 11; } // EAGAIN = 11
        // returns error code as string
        std::string_view str() const { return strerror(error_code); }
    };

    // Shortcut for a const-preserving `reinterpret_cast`ing c.data() from a std::byte to a uint8_t
    // pointer, because we need it all over the place in the ngtcp2 API
    template <
        typename Container,
        typename = std::enable_if_t<
            sizeof(typename std::remove_reference_t<Container>::value_type) == sizeof(uint8_t)>>
    inline auto*
    u8data(Container&& c)
    {
        using u8_sameconst_t = std::conditional_t<
            std::is_const_v<std::remove_pointer_t<decltype(c.data())>>,
            const uint8_t,
            uint8_t>;
        return reinterpret_cast<u8_sameconst_t*>(c.data());
    }


    // Namespacing for named arguments in API calls
    namespace opt
    {
        struct local_addr : public Address { using Address::Address; };
        struct remote_addr : public Address { using Address::Address; };

        /*
        TOFIX: templated TLS argument forwarding for client/server ctx creation

        template <typename TLSType_t>
        struct local_tls : TLSType_t { using TLSType_t::TLSType_t; };
        template <typename TLSType_t>
        struct remote_tls : TLSType_t { using TLSType_t::TLSType_t; };
        */
    }   // namespace oxen::quic::opt

}   // namespace oxen::quic


namespace std
{
    // Custom hash is required s.t. unordered_set storing ConnectionID:shared_ptr<Connection> 
    // is able to call its implicit constructor
    template <>
    struct hash<oxen::quic::ConnectionID>
    {
        size_t
        operator()(const oxen::quic::ConnectionID& cid) const
        {
            static_assert(
                alignof(oxen::quic::ConnectionID) >= alignof(size_t)
                && offsetof(oxen::quic::ConnectionID, data) % sizeof(size_t) == 0);
            return *reinterpret_cast<const size_t*>(cid.data);
        }
    };

    template <>
    struct hash<oxen::quic::Address>
    {
        size_t
        operator()(const oxen::quic::Address& addr) const
        {
            return std::hash<uint16_t>{}(addr.port);
        }
    };
}   // namespace std
