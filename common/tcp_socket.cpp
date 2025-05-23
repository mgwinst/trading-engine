#include "tcp_socket.h"

namespace Common {

    int TCPSocket::connect(const std::string& ip, const std::string& iface, int port, bool is_listening) {
        const SocketCfg socket_cfg{ip, iface, port, false, is_listening, true};
        socket_fd_ = create_socket(logger_, socket_cfg);
        
        // want to store peers IP/port info on connection, do we need this info when using facilities below? (read into this later...) (msghdr?)
        socket_attrib_.sin_addr.s_addr = INADDR_ANY;
        socket_attrib_.sin_family = AF_INET;
        socket_attrib_.sin_port = htons(port);

        return socket_fd_;
    }

    // handle non-blocking I/O for TCP socket
    bool TCPSocket::send_and_recv() noexcept {

        // control message buffer setup
        // allocate buffer to receive ancillary data (kernel timestamp) via recvmsg
        // sized to hold a control message with struct timeval payload (for timestamps) + cmsghdr and alignment padding
        // CMSG_SPACE is macro to calculate size for payload + cmsghdr + padding = 12 + 16 + 4 = 32

        char ctrl[CMSG_SPACE(sizeof(struct timeval))];        
        auto cmsg = reinterpret_cast<struct cmsghdr*>(&ctrl); 

        iovec iov{inbound_data_.data() + next_recv_valid_index_, TCP_BUFFER_SIZE - next_recv_valid_index_};
        msghdr msg{&socket_attrib_, sizeof(socket_attrib_), &iov, 1, ctrl, sizeof(ctrl), 0}; // address info, recv buffer (iov) and control buffer (ctrl)
        
        // non-blocking call to read available data
        // kernel checks sockets recv buffer (iov) if data available, writes read_size bytes to inbound_data_ at idx and timestamp is stored in ctrl
        const auto read_size = recvmsg(socket_fd_, &msg, MSG_DONTWAIT); // MSG_DONTWAIT -> makes recvmsg non-blocking

        if (read_size > 0) {
            next_recv_valid_index_ += read_size;

            Nanos kernel_time = 0;
            timeval time_kernel; // stuct timeval to hold timestamp
            if (cmsg->cmsg_level == SOL_SOCKET && // message at the socket level
                cmsg->cmsg_type == SCM_TIMESTAMP && // message is a timestamp
                cmsg->cmsg_len == CMSG_LEN(sizeof(time_kernel))) { // length == cmsghdr + struct timeval, CMSG_LEN does same at CMSG_SPACE but strips padding
                    memcpy(&time_kernel, CMSG_DATA(cmsg), sizeof(time_kernel));
                    kernel_time = time_kernel.tv_sec * 1000000000 + time_kernel.tv_usec * 1000;
            }

            const auto user_time = getCurrentNanos();

            logger_.log("%:% %() % read socket:% len:% utime:% ktime:% diff:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::get_current_time_str(time_str_), socket_fd_, next_recv_valid_index_, user_time, kernel_time, (user_time - kernel_time));

            recv_callback_(this, kernel_time); // look into this
        }

        // non-blocking call to send data
        if (next_send_valid_index_ > 0) {
            const auto n = ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_, MSG_DONTWAIT | MSG_NOSIGNAL); // POSIX send (::send)
            logger_.log("%:% %() % send socket:% len:%\n", __FILE__, __LINE__, __FUNCTION__, Common::get_current_time_str(time_str_), socket_fd_, n);
        }

        // this is incorrect if (n < next_send_valid_index_), potential loss of data if partial send
        // will support this later
        next_send_valid_index_ = 0;
        
        return (read_size > 0);
    }

    // write outgoing data to the send buffers
    void TCPSocket::send(const void* data, std::size_t len) noexcept {
        memcpy(outbound_data_.data() + next_send_valid_index_, data, len);
        next_send_valid_index_ += len;
    }

}