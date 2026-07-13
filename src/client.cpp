#include "fileshare/client.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "fileshare/checksum.hpp"

namespace fileshare {

void Client::connect(const std::string& host, std::uint16_t port) {
    sock_ = net::tcp_connect(host, port);
}

std::vector<ListEntry> Client::request_list() {
    net::send_all(sock_, encode_list_request());
    const std::optional<Frame> frame = net::recv_message(sock_);
    if (!frame) {
        throw net::NetError("server closed the connection during LIST");
    }
    if (frame->type == MessageType::ERROR_MSG) {
        const ErrorMessage e = parse_error(frame->payload.data(), frame->payload.size());
        throw ProtocolError("server error: " + e.message);
    }
    if (frame->type != MessageType::LIST_RESPONSE) {
        throw ProtocolError("unexpected reply to LIST_REQUEST");
    }
    return parse_list_response(frame->payload.data(), frame->payload.size());
}

Client::DownloadResult Client::download(const std::string& alias, const std::string& out_path,
                                        std::uint64_t expected_size, const ProgressFn& progress) {
    DownloadResult res;

    net::send_all(sock_, encode_download_request(DownloadRequest{alias, 0}));

    // Stream into a temp file and only rename onto out_path on full success, so
    // a failed download (FILE_NOT_FOUND, mid-transfer drop, checksum mismatch)
    // never truncates or clobbers an existing destination.
    const std::string tmp_path = out_path + ".part";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        res.error = "cannot open temp file: " + tmp_path;
        return res;
    }

    FileHasher hasher;
    std::uint64_t received = 0;

    // Close and delete the temp file, then return `res` with an error set.
    auto fail = [&](std::string message) -> DownloadResult {
        out.close();
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        res.error = std::move(message);
        res.bytes = received;
        return res;
    };

    for (;;) {
        const std::optional<Frame> frame = net::recv_message(sock_);
        if (!frame) {
            return fail("connection closed mid-download");
        }
        switch (frame->type) {
            case MessageType::CHUNK_DATA: {
                if (!frame->payload.empty()) {
                    out.write(reinterpret_cast<const char*>(frame->payload.data()),
                              static_cast<std::streamsize>(frame->payload.size()));
                    if (!out) {
                        return fail("write error on output file");
                    }
                    hasher.update(frame->payload.data(), frame->payload.size());
                    received += frame->payload.size();
                }
                if (progress) {
                    progress(received, expected_size);
                }
                break;
            }
            case MessageType::DOWNLOAD_DONE: {
                const Checksum server_sum =
                    parse_download_done(frame->payload.data(), frame->payload.size());
                const Checksum local_sum = hasher.value();
                if (server_sum != local_sum) {
                    auto r = fail("checksum mismatch");
                    return r; // res.ok stays false; destination untouched
                }
                out.flush();
                out.close();
                if (!out) {   // a disk error surfacing only at flush/close must not read as success
                    return fail("error flushing output file (disk full?)");
                }
                std::error_code ec;
                std::filesystem::rename(tmp_path, out_path, ec);
                if (ec) {
                    return fail("cannot move download into place: " + ec.message());
                }
                res.ok = true;
                res.bytes = received;
                res.checksum_ok = true;
                return res;
            }
            case MessageType::ERROR_MSG: {
                const ErrorMessage e = parse_error(frame->payload.data(), frame->payload.size());
                return fail("server error: " + e.message);
            }
            default:
                return fail("unexpected message type during download");
        }
    }
}

} // namespace fileshare
