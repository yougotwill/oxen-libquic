#pragma once

#include "utils.hpp"

#include <ngtcp2/ngtcp2.h>

#include <cassert>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <deque>


namespace oxen::quic
{
	class Connection;
	class Stream;

	using data_callback_t = std::function<void(Stream&, bstring)>;
	using close_callback_t = std::function<void(Stream&, uint64_t error_code)>;

	///	One-shot datagram sent inside a quic connection
	struct DatagramBuffer
	{
		public:
			///	Write buffer for outgoing packets
			std::vector<std::byte> buf;

            /// Returns number of bits written to buffer
			size_t
			size() const { return buf.size(); }

			explicit DatagramBuffer(size_t size = 1200)
			{
				buf.reserve(size);
                nwrote = 0;
                remaining = size;
			}

			~DatagramBuffer()
			{
				std::memset(&buf, 0, buf.size());
                buf.clear();
			}

			DatagramBuffer&
			operator=(const DatagramBuffer& d)
			{
                assert(d.size() <= size());
                std::memcpy(buf.data(), d.buf.data(), d.size());
                return *this;
			}

            size_t
            write(const char* data, size_t nbits);

		private:
			///	Bits written to buffer
			size_t nwrote;
            size_t remaining;
	};


	class Stream : public std::enable_shared_from_this<Stream>
	{
		public:
			Stream(Connection& conn, data_callback_t data_cb, close_callback_t close_cb, size_t bufsize, int64_t stream_id = -1);
			Stream(Connection& conn, int64_t stream_id, size_t bufsize);

			data_callback_t data_callback;
			close_callback_t close_callback;

			int64_t stream_id;
			
			std::vector<std::byte> buf{65536};
			std::deque<std::pair<std::unique_ptr<const std::byte[]>, size_t>> user_buffers;

			size_t datalen;
			size_t nwrite;

			Connection&
			get_conn();

			void
			close(uint64_t error_code = 0);

			void
			wrote(size_t bytes);

			void
			append_buffer(const std::byte* buffer, size_t length);

			void
			acknowledge(size_t bytes);

			inline size_t
			available() const
			{ return is_closing || buf.empty() ? 0 : buf.size() - size; }

			inline size_t
			used() const
			{ return size; }

			inline size_t
			unacked() const
			{ return unacked_size; }

			inline size_t
			unsent() const
			{ return used() - unacked(); }
		
		private:
			friend class Connection;

			Connection& conn;

			std::vector<bstring>
			pending();

			size_t size{0};
			size_t start{0};
			size_t unacked_size{0};

			bool is_new{false};
			bool is_closing{false};
			bool is_shutdown{false};
			bool sent_fin{false};
	};

	void quic_stream_destroy(Stream* stream);
	int quic_stream_send(Stream* stream, const void *data, size_t data_len);

}	// namespace oxen::quic
