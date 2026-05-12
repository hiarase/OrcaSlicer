#include "SnapmakerJ1.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Slic3r {
namespace {

struct HttpResult
{
    bool success { false };
    unsigned status { 0 };
    std::string body;
    std::string error;
    nlohmann::json json;
};

static std::string make_url_from_host(const std::string& host, const std::string& path)
{
    std::string normalized_path = path;
    while (!normalized_path.empty() && normalized_path.front() == '/')
        normalized_path.erase(normalized_path.begin());

    if (boost::starts_with(host, "http://") || boost::starts_with(host, "https://")) {
        return host.back() == '/' ? host + normalized_path : host + "/" + normalized_path;
    }

    const std::string host_with_port = host.find(':') == std::string::npos ? host + ":8080" : host;
    return "http://" + host_with_port + "/" + normalized_path;
}

static std::string make_form_body(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string body;
    for (const auto& field : fields) {
        if (!body.empty())
            body += "&";
        body += Http::url_encode(field.first) + "=" + Http::url_encode(field.second);
    }
    return body;
}

static nlohmann::json parse_body(const std::string& body)
{
    if (body.empty())
        return nlohmann::json::object();

    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_discarded())
        return parsed;

    nlohmann::json result;
    result["text"] = body;
    return result;
}

static nlohmann::json to_callback_json(const HttpResult& result)
{
    nlohmann::json response;
    response["success"] = result.success;
    response["http_status"] = result.status;
    if (!result.error.empty())
        response["error"] = result.error;
    if (!result.body.empty())
        response["text"] = result.body;
    if (!result.json.is_null())
        response["data"] = result.json;
    return response;
}

static void apply_http_options(Http& http, const std::string& cafile, bool ssl_revoke_best_effort)
{
    if (!cafile.empty())
        http.ca_file(cafile);
#ifdef WIN32
    http.ssl_revoke_best_effort(ssl_revoke_best_effort);
#else
    (void) ssl_revoke_best_effort;
#endif
}

static HttpResult perform_post_form(const std::string& host, const std::string& cafile, bool ssl_revoke_best_effort,
                                    const std::string& path, const std::vector<std::pair<std::string, std::string>>& fields,
                                    long timeout)
{
    HttpResult result;
    auto http = Http::post(make_url_from_host(host, path));
    apply_http_options(http, cafile, ssl_revoke_best_effort);

    http.header("Content-Type", "application/x-www-form-urlencoded")
        .set_post_body(make_form_body(fields))
        .timeout_max(timeout)
        .on_complete([&result](std::string body, unsigned status) {
            result.success = true;
            result.status = status;
            result.body = std::move(body);
            result.json = parse_body(result.body);
        })
        .on_error([&result](std::string body, std::string error, unsigned status) {
            result.success = false;
            result.status = status;
            result.body = std::move(body);
            result.error = std::move(error);
            result.json = parse_body(result.body);
        })
        .perform_sync();

    return result;
}

static HttpResult perform_get_json(const std::string& host, const std::string& cafile, bool ssl_revoke_best_effort,
                                   const std::string& path, long timeout)
{
    HttpResult result;
    auto http = Http::get(make_url_from_host(host, path));
    apply_http_options(http, cafile, ssl_revoke_best_effort);

    http.timeout_max(timeout)
        .on_complete([&result](std::string body, unsigned status) {
            result.success = true;
            result.status = status;
            result.body = std::move(body);
            result.json = parse_body(result.body);
        })
        .on_error([&result](std::string body, std::string error, unsigned status) {
            result.success = false;
            result.status = status;
            result.body = std::move(body);
            result.error = std::move(error);
            result.json = parse_body(result.body);
        })
        .perform_sync();

    return result;
}

static std::string get_string_if_exists(const nlohmann::json& json, const char* key)
{
    if (!json.is_object() || !json.contains(key))
        return {};
    if (json[key].is_string())
        return json[key].get<std::string>();
    return {};
}

static void run_async_post(const std::string& host, const std::string& cafile, bool ssl_revoke_best_effort,
                           const std::string& path, std::vector<std::pair<std::string, std::string>> fields,
                           std::function<void(const nlohmann::json&)> callback, long timeout = 120)
{
    std::thread([host, cafile, ssl_revoke_best_effort, path, fields = std::move(fields), callback = std::move(callback), timeout]() mutable {
        auto result = perform_post_form(host, cafile, ssl_revoke_best_effort, path, fields, timeout);
        if (callback)
            callback(to_callback_json(result));
    }).detach();
}

static void run_async_get(const std::string& host, const std::string& cafile, bool ssl_revoke_best_effort, const std::string& path,
                          std::function<void(const nlohmann::json&)> callback, long timeout = 30)
{
    std::thread([host, cafile, ssl_revoke_best_effort, path, callback = std::move(callback), timeout]() mutable {
        auto result = perform_get_json(host, cafile, ssl_revoke_best_effort, path, timeout);
        if (callback)
            callback(to_callback_json(result));
    }).detach();
}

using Bytes = std::vector<unsigned char>;

struct SacpEndpoint
{
    std::string host;
    uint16_t port { 8888 };
};

struct SacpPacket
{
    uint8_t receiver { 0 };
    uint8_t sender { 0 };
    uint8_t attribute { 0 };
    uint16_t sequence { 0 };
    uint8_t command_set { 0 };
    uint8_t command_id { 0 };
    Bytes payload;
};

struct SacpResponse
{
    bool success { false };
    uint8_t result { 0 };
    Bytes data;
    std::string error;
};

static constexpr uint8_t SACP_PEER_LUBAN      = 0;
static constexpr uint8_t SACP_PEER_CONTROLLER = 1;
static constexpr uint8_t SACP_PEER_SCREEN     = 2;
static constexpr uint8_t SACP_ATTR_REQUEST    = 0;
static constexpr uint8_t SACP_ATTR_ACK        = 1;

static void append_u8(Bytes& buffer, uint8_t value) { buffer.push_back(value); }

static void append_u16_le(Bytes& buffer, uint16_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

static void append_i16_le(Bytes& buffer, int16_t value)
{
    append_u16_le(buffer, static_cast<uint16_t>(value));
}

static void append_u32_le(Bytes& buffer, uint32_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

static uint16_t read_u16_le(const Bytes& buffer, size_t offset)
{
    if (offset + 2 > buffer.size())
        return 0;
    return static_cast<uint16_t>(buffer[offset] | (buffer[offset + 1] << 8));
}

static int16_t read_i16_le(const Bytes& buffer, size_t offset)
{
    return static_cast<int16_t>(read_u16_le(buffer, offset));
}

static uint32_t read_u32_le(const Bytes& buffer, size_t offset)
{
    if (offset + 4 > buffer.size())
        return 0;
    return static_cast<uint32_t>(buffer[offset]) | (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
           (static_cast<uint32_t>(buffer[offset + 2]) << 16) | (static_cast<uint32_t>(buffer[offset + 3]) << 24);
}

static int32_t read_i32_le(const Bytes& buffer, size_t offset)
{
    return static_cast<int32_t>(read_u32_le(buffer, offset));
}

static double read_sacp_float(const Bytes& buffer, size_t offset)
{
    return static_cast<double>(read_i32_le(buffer, offset)) / 1000.0;
}

static void append_sacp_string(Bytes& buffer, const std::string& value)
{
    append_u16_le(buffer, static_cast<uint16_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
}

static void append_sacp_binary(Bytes& buffer, const Bytes& value)
{
    append_u16_le(buffer, static_cast<uint16_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
}

static bool read_sacp_string(const Bytes& buffer, size_t offset, std::string& value, size_t& next_offset)
{
    if (offset + 2 > buffer.size())
        return false;
    const uint16_t length = read_u16_le(buffer, offset);
    if (offset + 2 + length > buffer.size())
        return false;
    value.assign(reinterpret_cast<const char*>(buffer.data() + offset + 2), length);
    next_offset = offset + 2 + length;
    return true;
}

static uint8_t calc_sacp_crc8(const Bytes& buffer, size_t offset, size_t length)
{
    uint8_t crc = 0x00;
    constexpr uint8_t poly = 0x07;
    for (size_t i = offset; i < offset + length; ++i) {
        for (int j = 0; j < 8; ++j) {
            const bool bit = ((buffer[i] >> (7 - j)) & 1) == 1;
            const bool c07 = ((crc >> 7) & 1) == 1;
            crc <<= 1;
            if (c07 != bit)
                crc ^= poly;
        }
    }
    return crc;
}

static uint16_t calc_sacp_checksum(const Bytes& buffer, size_t offset, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2)
        sum += (static_cast<uint32_t>(buffer[offset + i]) << 8) + buffer[offset + i + 1];
    if ((length & 1) != 0)
        sum += buffer[offset + length - 1];
    while ((sum >> 16) > 0)
        sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>((~sum) & 0xffff);
}

static SacpEndpoint parse_sacp_endpoint(std::string host)
{
    boost::trim(host);
    const size_t scheme = host.find("://");
    if (scheme != std::string::npos)
        host.erase(0, scheme + 3);
    const size_t at = host.find('@');
    if (at != std::string::npos)
        host.erase(0, at + 1);
    const size_t slash = host.find_first_of("/?#");
    if (slash != std::string::npos)
        host.erase(slash);

    if (!host.empty() && host.front() == '[') {
        const size_t closing = host.find(']');
        if (closing != std::string::npos)
            host = host.substr(1, closing - 1);
    } else {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon)
            host.erase(colon);
    }

    return { host, 8888 };
}

static std::string local_hostname()
{
#ifndef _WIN32
    std::array<char, 256> hostname {};
    if (::gethostname(hostname.data(), hostname.size() - 1) == 0 && hostname[0] != '\0')
        return hostname.data();
#endif
    return "OrcaSlicer";
}

static std::string sacp_result_message(uint8_t result)
{
    if (result == 0)
        return {};
    std::ostringstream message;
    message << "SACP result " << static_cast<int>(result);
    return message.str();
}

static nlohmann::json sacp_response_json(const SacpResponse& response)
{
    nlohmann::json json;
    json["success"] = response.success && response.result == 0;
    json["sacp"] = true;
    json["sacp_result"] = response.result;
    if (!response.error.empty())
        json["error"] = response.error;
    return json;
}

static std::string workflow_status_name(uint8_t status)
{
    switch (status) {
    case 0: return "idle";
    case 1: return "starting";
    case 2: return "running";
    case 3: return "pausing";
    case 4: return "paused";
    case 5: return "stopping";
    case 6: return "stopped";
    case 7: return "finishing";
    case 8: return "completed";
    case 9: return "recovering";
    case 10: return "resuming";
    default: return "unknown";
    }
}

class SacpTcpClient
{
public:
    SacpTcpClient(std::string host, std::string token)
        : m_endpoint(parse_sacp_endpoint(std::move(host)))
        , m_token(std::move(token))
    {}

    ~SacpTcpClient() { close_socket(); }

    bool connect(wxString* msg)
    {
        if (m_endpoint.host.empty()) {
            set_error("Missing Snapmaker J1 host.");
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }

        if (!open_socket(3000)) {
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        Bytes payload;
        append_sacp_string(payload, local_hostname());
        append_sacp_string(payload, "OrcaSlicer");
        append_sacp_string(payload, m_token);

        SacpResponse response = send_request(0x01, 0x05, SACP_PEER_SCREEN, payload, 5000);
        if (!response.success || response.result != 0) {
            set_error(response.error.empty() ? sacp_result_message(response.result) : response.error);
            if (msg)
                *msg = GUI::from_u8(m_error);
            close_socket();
            return false;
        }

        m_connected = true;
        send_request(0xb0, 0x0b, SACP_PEER_SCREEN, Bytes(), 2000);
        return true;
    }

    void close()
    {
        if (is_open())
            send_request(0x01, 0x06, SACP_PEER_SCREEN, Bytes(), 1000);
        close_socket();
    }

    const std::string& error() const { return m_error; }

    std::string url_label() const
    {
        std::ostringstream label;
        label << "sacp://" << m_endpoint.host << ":" << m_endpoint.port;
        return label.str();
    }

    SacpResponse execute_gcode(const std::string& gcode)
    {
        Bytes payload;
        append_sacp_string(payload, gcode);
        return send_request(0x01, 0x02, SACP_PEER_CONTROLLER, payload, 300000);
    }

    SacpResponse stop_print() { return send_request(0xac, 0x06, SACP_PEER_CONTROLLER, Bytes(), 120000); }
    SacpResponse pause_print() { return send_request(0xac, 0x04, SACP_PEER_CONTROLLER, Bytes(), 120000); }
    SacpResponse resume_print() { return send_request(0xac, 0x05, SACP_PEER_CONTROLLER, Bytes(), 120000); }

    SacpResponse start_screen_print(const std::string& filename, const std::string& md5)
    {
        Bytes payload;
        append_u8(payload, 0);
        append_sacp_string(payload, filename);
        append_sacp_string(payload, md5);
        return send_request(0xb0, 0x08, SACP_PEER_SCREEN, payload, 120000);
    }

    bool upload_file(const boost::filesystem::path& source_path, const std::string& render_name, PrintHost::ProgressFn progress_fn,
                     wxString* msg, std::string* md5_out)
    {
        if (!boost::filesystem::exists(source_path)) {
            set_error("Missing file: " + source_path.string());
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }

        const uint64_t file_size_64 = boost::filesystem::file_size(source_path);
        if (file_size_64 > std::numeric_limits<uint32_t>::max()) {
            set_error("SACP upload supports files smaller than 4 GB.");
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }

        std::string path_string = source_path.string();
        std::string md5;
        if (!bbl_calc_md5(path_string, md5)) {
            set_error("Failed to calculate file MD5.");
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }
        std::transform(md5.begin(), md5.end(), md5.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (md5_out)
            *md5_out = md5;

        constexpr uint32_t chunk_size = 60 * 1024;
        const uint32_t file_size = static_cast<uint32_t>(file_size_64);
        const uint16_t chunks = static_cast<uint16_t>((file_size + chunk_size - 1) / chunk_size);

        Bytes payload;
        append_sacp_string(payload, render_name);
        append_u32_le(payload, file_size);
        append_u16_le(payload, chunks);
        append_sacp_string(payload, md5);

        SacpResponse initial = send_request(0xb0, 0x00, SACP_PEER_SCREEN, payload, 120000);
        if (!initial.success || initial.result != 0) {
            set_error(initial.error.empty() ? sacp_result_message(initial.result) : initial.error);
            if (msg)
                *msg = GUI::from_u8(m_error);
            return false;
        }

        bool done = false;
        bool success = false;
        const auto started = std::chrono::steady_clock::now();
        while (!done) {
            if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
                set_error("SACP upload timed out.");
                break;
            }

            SacpPacket packet;
            if (!read_packet(packet, 30000)) {
                set_error(m_error.empty() ? "SACP upload timed out." : m_error);
                break;
            }

            if (packet.attribute != SACP_ATTR_REQUEST) {
                handle_default_request(packet);
                continue;
            }

            if (packet.command_set == 0xb0 && packet.command_id == 0x01) {
                std::string requested_md5;
                size_t next_offset = 0;
                if (!read_sacp_string(packet.payload, 0, requested_md5, next_offset) || next_offset + 2 > packet.payload.size()) {
                    ack(packet, Bytes { 1 });
                    continue;
                }

                const uint16_t index = read_u16_le(packet.payload, next_offset);
                const uint64_t offset = static_cast<uint64_t>(index) * chunk_size;
                const uint64_t remaining = offset < file_size_64 ? file_size_64 - offset : 0;
                const size_t to_read = static_cast<size_t>(std::min<uint64_t>(chunk_size, remaining));

                Bytes chunk;
                if (requested_md5 != md5 || !read_file_chunk(source_path, offset, to_read, chunk)) {
                    ack(packet, Bytes { 200 });
                    continue;
                }

                Bytes response;
                append_u8(response, 0);
                append_sacp_string(response, requested_md5);
                append_u16_le(response, index);
                append_sacp_binary(response, chunk);
                ack(packet, response);

                if (progress_fn) {
                    bool cancel = false;
                    const uint64_t uploaded = std::min<uint64_t>(file_size_64, offset + chunk.size());
                    const std::string empty;
                    Http::Progress progress(0, 0, static_cast<size_t>(file_size_64), static_cast<size_t>(uploaded), empty);
                    progress_fn(std::move(progress), cancel);
                    if (cancel) {
                        set_error("Upload canceled.");
                        return false;
                    }
                }
            } else if (packet.command_set == 0xb0 && packet.command_id == 0x02) {
                const uint8_t result = packet.payload.empty() ? 1 : packet.payload[0];
                ack(packet, Bytes { 0 });
                success = result == 0;
                done = true;
                if (!success)
                    set_error(sacp_result_message(result));
            } else {
                handle_default_request(packet);
            }
        }

        if (!success && msg)
            *msg = GUI::from_u8(m_error);
        return success;
    }

    nlohmann::json get_device_status()
    {
        nlohmann::json data;
        data["machineName"] = "Snapmaker J1";
        data["series"] = "Snapmaker J1";
        data["status"] = "unknown";

        SacpResponse machine = send_request(0x01, 0x21, SACP_PEER_CONTROLLER, Bytes(), 5000);
        if (machine.success && machine.result == 0 && machine.data.size() >= 6) {
            const uint8_t machine_type = machine.data[0];
            data["machineType"] = machine_type;
            std::string firmware;
            size_t next = 0;
            if (read_sacp_string(machine.data, 6, firmware, next))
                data["firmwareVersion"] = firmware;
        }

        int print_head_key = -1;
        int heated_bed_key = -1;
        SacpResponse modules = send_request(0x01, 0x20, SACP_PEER_CONTROLLER, Bytes(), 5000);
        if (modules.success && modules.result == 0)
            parse_module_info(modules.data, print_head_key, heated_bed_key);

        if (print_head_key >= 0) {
            Bytes payload;
            append_u8(payload, static_cast<uint8_t>(print_head_key));
            SacpResponse fdm = send_request(0x10, 0x01, SACP_PEER_CONTROLLER, payload, 5000);
            if (fdm.success && fdm.result == 0)
                apply_fdm_info(data, fdm.data);
        }

        if (heated_bed_key >= 0) {
            Bytes payload;
            append_u8(payload, static_cast<uint8_t>(heated_bed_key));
            SacpResponse bed = send_request(0x14, 0x01, SACP_PEER_CONTROLLER, payload, 5000);
            if (bed.success && bed.result == 0)
                apply_hotbed_info(data, bed.data);
        }

        SacpResponse heartbeat = subscribe_once(0x01, 0xa0, 1000, 1800);
        uint8_t workflow = 255;
        if (heartbeat.success && heartbeat.result == 0 && !heartbeat.data.empty()) {
            workflow = heartbeat.data[0];
            data["status"] = workflow_status_name(workflow);
            data["statusCode"] = workflow;
        }

        SacpResponse file_info = send_request(0xac, 0x1a, SACP_PEER_SCREEN, Bytes { 0 }, 5000);
        uint32_t total_lines = 0;
        uint32_t estimated_time = 0;
        if (file_info.success && file_info.result == 0) {
            std::string filename;
            size_t next = 0;
            if (read_sacp_string(file_info.data, 0, filename, next) && next + 8 <= file_info.data.size()) {
                total_lines = read_u32_le(file_info.data, next);
                estimated_time = read_u32_le(file_info.data, next + 4);
                data["filename"] = filename;
                data["totalLines"] = total_lines;
                data["estimatedTime"] = estimated_time;
            }
        }

        if (workflow == 2 || workflow == 3 || workflow == 4 || workflow == 7 || workflow == 9 || workflow == 10) {
            SacpResponse current_line = subscribe_once(0xac, 0xa0, 1000, 1800);
            if (current_line.success && current_line.result == 0 && current_line.data.size() >= 4) {
                const uint32_t line = read_u32_le(current_line.data, 0);
                data["currentLine"] = line;
                if (total_lines > 0)
                    data["progress"] = static_cast<double>(line) / static_cast<double>(total_lines);
            }

            SacpResponse elapsed = subscribe_once(0xac, 0xa5, 1000, 1800);
            if (elapsed.success && elapsed.result == 0 && elapsed.data.size() >= 4) {
                const uint32_t elapsed_seconds = read_u32_le(elapsed.data, 0);
                data["elapsedTime"] = elapsed_seconds;
                if (estimated_time > elapsed_seconds)
                    data["remainingTime"] = estimated_time - elapsed_seconds;
            }
        }

        nlohmann::json response;
        response["success"] = true;
        response["sacp"] = true;
        response["data"] = std::move(data);
        return response;
    }

    SacpResponse set_bed_temperature(int temp)
    {
        int print_head_key = -1;
        int heated_bed_key = -1;
        SacpResponse modules = send_request(0x01, 0x20, SACP_PEER_CONTROLLER, Bytes(), 5000);
        if (modules.success && modules.result == 0)
            parse_module_info(modules.data, print_head_key, heated_bed_key);
        if (heated_bed_key < 0)
            heated_bed_key = 0;

        Bytes payload;
        append_u8(payload, static_cast<uint8_t>(heated_bed_key));
        append_u8(payload, 0);
        append_i16_le(payload, static_cast<int16_t>(temp));
        return send_request(0x14, 0x02, SACP_PEER_CONTROLLER, payload, 30000);
    }

    SacpResponse set_extruder_temperature(int temp, int index)
    {
        int print_head_key = -1;
        int heated_bed_key = -1;
        SacpResponse modules = send_request(0x01, 0x20, SACP_PEER_CONTROLLER, Bytes(), 5000);
        if (modules.success && modules.result == 0)
            parse_module_info(modules.data, print_head_key, heated_bed_key);
        if (print_head_key < 0)
            print_head_key = 0;

        const uint8_t extruder_index = static_cast<uint8_t>(std::max(0, std::min(index, 1)));
        Bytes switch_payload;
        append_u8(switch_payload, static_cast<uint8_t>(print_head_key));
        append_u8(switch_payload, extruder_index);
        send_request(0x10, 0x05, SACP_PEER_CONTROLLER, switch_payload, 30000);

        Bytes payload;
        append_u8(payload, static_cast<uint8_t>(print_head_key));
        append_u8(payload, extruder_index);
        append_i16_le(payload, static_cast<int16_t>(temp));
        return send_request(0x10, 0x02, SACP_PEER_CONTROLLER, payload, 30000);
    }

private:
    bool is_open() const
    {
#ifndef _WIN32
        return m_fd >= 0;
#else
        return false;
#endif
    }

    void set_error(std::string error)
    {
        m_error = std::move(error);
        if (m_error.empty())
            m_error = "SACP connection failed.";
    }

    void close_socket()
    {
#ifndef _WIN32
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
        m_connected = false;
    }

    bool open_socket(int timeout_ms)
    {
#ifdef _WIN32
        set_error("SACP TCP is not available in this build.");
        return false;
#else
        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* addresses = nullptr;
        const std::string port = std::to_string(m_endpoint.port);
        const int gai = ::getaddrinfo(m_endpoint.host.c_str(), port.c_str(), &hints, &addresses);
        if (gai != 0) {
            set_error(std::string("Could not resolve ") + m_endpoint.host + ": " + ::gai_strerror(gai));
            return false;
        }

        for (struct addrinfo* ai = addresses; ai; ai = ai->ai_next) {
            const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;

            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc < 0 && errno == EINPROGRESS) {
                if (wait_fd(fd, true, timeout_ms)) {
                    int socket_error = 0;
                    socklen_t len = sizeof(socket_error);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0 && socket_error == 0)
                        rc = 0;
                    else
                        errno = socket_error;
                }
            }

            if (rc == 0) {
                m_fd = fd;
                ::freeaddrinfo(addresses);
                return true;
            }

            ::close(fd);
        }

        ::freeaddrinfo(addresses);
        set_error("Could not connect to " + m_endpoint.host + ":8888.");
        return false;
#endif
    }

#ifndef _WIN32
    bool wait_fd(int fd, bool write, int timeout_ms)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);

        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int rc = ::select(fd + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
        return rc > 0;
    }
#endif

    bool write_all(const Bytes& buffer, int timeout_ms)
    {
#ifdef _WIN32
        (void) buffer;
        (void) timeout_ms;
        return false;
#else
        size_t written = 0;
        while (written < buffer.size()) {
            if (!wait_fd(m_fd, true, timeout_ms)) {
                set_error("SACP write timed out.");
                return false;
            }
            const ssize_t rc = ::send(m_fd, buffer.data() + written, buffer.size() - written, 0);
            if (rc > 0) {
                written += static_cast<size_t>(rc);
                continue;
            }
            if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            set_error(std::string("SACP write failed: ") + std::strerror(errno));
            return false;
        }
        return true;
#endif
    }

    bool read_exact(Bytes& buffer, size_t count, int timeout_ms)
    {
#ifdef _WIN32
        (void) buffer;
        (void) count;
        (void) timeout_ms;
        return false;
#else
        const size_t start = buffer.size();
        buffer.resize(start + count);
        size_t read = 0;
        while (read < count) {
            if (!wait_fd(m_fd, false, timeout_ms)) {
                buffer.resize(start + read);
                set_error("SACP read timed out.");
                return false;
            }
            const ssize_t rc = ::recv(m_fd, buffer.data() + start + read, count - read, 0);
            if (rc > 0) {
                read += static_cast<size_t>(rc);
                continue;
            }
            if (rc == 0) {
                buffer.resize(start + read);
                set_error("SACP connection closed.");
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            buffer.resize(start + read);
            set_error(std::string("SACP read failed: ") + std::strerror(errno));
            return false;
        }
        return true;
#endif
    }

    bool read_byte(uint8_t& byte, int timeout_ms)
    {
        Bytes buffer;
        if (!read_exact(buffer, 1, timeout_ms))
            return false;
        byte = buffer[0];
        return true;
    }

    Bytes build_packet(uint8_t command_set, uint8_t command_id, uint8_t receiver, uint8_t attribute, uint16_t sequence, const Bytes& payload)
    {
        Bytes packet;
        packet.reserve(15 + payload.size());
        append_u8(packet, 0xaa);
        append_u8(packet, 0x55);
        append_u16_le(packet, static_cast<uint16_t>(payload.size() + 8));
        append_u8(packet, 0x01);
        append_u8(packet, receiver);
        append_u8(packet, 0x00);
        packet[6] = calc_sacp_crc8(packet, 0, 6);
        append_u8(packet, SACP_PEER_LUBAN);
        append_u8(packet, attribute);
        append_u16_le(packet, sequence);
        append_u8(packet, command_set);
        append_u8(packet, command_id);
        packet.insert(packet.end(), payload.begin(), payload.end());

        const uint16_t checksum = calc_sacp_checksum(packet, 7, packet.size() - 7);
        append_u16_le(packet, checksum);
        return packet;
    }

    bool send_packet(uint8_t command_set, uint8_t command_id, uint8_t receiver, uint8_t attribute, uint16_t sequence, const Bytes& payload,
                     int timeout_ms = 5000)
    {
        return write_all(build_packet(command_set, command_id, receiver, attribute, sequence, payload), timeout_ms);
    }

    bool read_packet(SacpPacket& packet, int timeout_ms)
    {
        uint8_t byte = 0;
        while (read_byte(byte, timeout_ms)) {
            if (byte != 0xaa)
                continue;
            uint8_t second = 0;
            if (!read_byte(second, timeout_ms))
                return false;
            if (second != 0x55)
                continue;

            Bytes raw { 0xaa, 0x55 };
            if (!read_exact(raw, 5, timeout_ms))
                return false;
            if (calc_sacp_crc8(raw, 0, 6) != raw[6])
                continue;

            const uint16_t length = read_u16_le(raw, 2);
            if (length < 8) {
                set_error("Invalid SACP packet length.");
                return false;
            }
            if (!read_exact(raw, length, timeout_ms))
                return false;

            if (calc_sacp_checksum(raw, 7, raw.size() - 9) != read_u16_le(raw, raw.size() - 2)) {
                set_error("Invalid SACP checksum.");
                return false;
            }

            packet.receiver = raw[5];
            packet.sender = raw[7];
            packet.attribute = raw[8];
            packet.sequence = read_u16_le(raw, 9);
            packet.command_set = raw[11];
            packet.command_id = raw[12];
            packet.payload.assign(raw.begin() + 13, raw.end() - 2);
            return true;
        }
        return false;
    }

    SacpResponse packet_response(const SacpPacket& packet) const
    {
        SacpResponse response;
        response.success = true;
        response.result = packet.payload.empty() ? 0 : packet.payload[0];
        if (packet.payload.size() > 1)
            response.data.assign(packet.payload.begin() + 1, packet.payload.end());
        if (response.result != 0)
            response.error = sacp_result_message(response.result);
        return response;
    }

    SacpResponse send_request(uint8_t command_set, uint8_t command_id, uint8_t peer, const Bytes& payload, int timeout_ms)
    {
        SacpResponse response;
        const uint16_t sequence = next_sequence();
        if (!send_packet(command_set, command_id, peer, SACP_ATTR_REQUEST, sequence, payload, timeout_ms)) {
            response.error = m_error;
            return response;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            SacpPacket packet;
            if (!read_packet(packet, std::max(1, remaining_ms))) {
                response.error = m_error;
                return response;
            }

            if (packet.attribute == SACP_ATTR_ACK && packet.sequence == sequence && packet.command_set == command_set && packet.command_id == command_id)
                return packet_response(packet);

            handle_default_request(packet);
        }

        response.error = "SACP request timed out.";
        return response;
    }

    SacpResponse subscribe_once(uint8_t command_set, uint8_t command_id, uint16_t interval, int timeout_ms)
    {
        Bytes payload;
        append_u8(payload, command_set);
        append_u8(payload, command_id);
        append_u16_le(payload, interval);

        SacpResponse subscribed = send_request(0x01, 0x00, SACP_PEER_CONTROLLER, payload, 5000);
        if (!subscribed.success || subscribed.result != 0)
            return subscribed;

        SacpResponse response;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            SacpPacket packet;
            if (!read_packet(packet, std::max(1, remaining_ms))) {
                response.error = m_error;
                return response;
            }

            if (packet.command_set == command_set && packet.command_id == command_id)
                return packet_response(packet);

            handle_default_request(packet);
        }

        response.error = "SACP subscription timed out.";
        return response;
    }

    bool ack(const SacpPacket& request, const Bytes& payload)
    {
        return send_packet(request.command_set, request.command_id, request.sender, SACP_ATTR_ACK, request.sequence, payload, 5000);
    }

    void handle_default_request(const SacpPacket& packet)
    {
        if (packet.attribute == SACP_ATTR_REQUEST && packet.command_set == 0x01 && packet.command_id == 0x06) {
            Bytes response { 0 };
            ack(packet, response);
        }
    }

    uint16_t next_sequence()
    {
        ++m_sequence;
        m_sequence %= 0xffff;
        if (m_sequence == 0)
            ++m_sequence;
        return m_sequence;
    }

    bool read_file_chunk(const boost::filesystem::path& path, uint64_t offset, size_t length, Bytes& chunk)
    {
        boost::nowide::ifstream stream(path.string(), std::ios::binary);
        if (!stream)
            return false;
        stream.seekg(static_cast<std::streamoff>(offset));
        if (!stream && length > 0)
            return false;
        chunk.resize(length);
        stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(length));
        chunk.resize(static_cast<size_t>(stream.gcount()));
        return chunk.size() == length;
    }

    void parse_module_info(const Bytes& data, int& print_head_key, int& heated_bed_key)
    {
        if (data.empty())
            return;
        const uint8_t count = data[0];
        size_t offset = 1;
        for (uint8_t i = 0; i < count && offset + 10 <= data.size(); ++i) {
            const uint8_t key = data[offset];
            const uint16_t module_id = read_u16_le(data, offset + 1);
            size_t version_next = 0;
            std::string version;
            if (!read_sacp_string(data, offset + 10, version, version_next))
                break;

            if (module_id == 0 || module_id == 13)
                print_head_key = key;
            else if (module_id == 513 || module_id == 512 || module_id == 515)
                heated_bed_key = key;
            offset = version_next;
        }
    }

    void apply_fdm_info(nlohmann::json& data, const Bytes& fdm)
    {
        if (fdm.size() < 4)
            return;
        const uint8_t count = fdm[3];
        size_t offset = 4;
        for (uint8_t i = 0; i < count && offset + 17 <= fdm.size(); ++i, offset += 17) {
            const uint8_t index = fdm[offset];
            const double current = read_sacp_float(fdm, offset + 9);
            const double target = read_sacp_float(fdm, offset + 13);
            const std::string suffix = index == 0 ? "1" : "2";
            data["nozzleTemperature" + suffix] = current;
            data["nozzleTargetTemperature" + suffix] = target;
            if (index == 0) {
                data["nozzleTemperature"] = current;
                data["nozzleTargetTemperature"] = target;
            } else {
                data["nozzleRightTemperature"] = current;
                data["nozzleRightTargetTemperature"] = target;
            }
        }
    }

    void apply_hotbed_info(nlohmann::json& data, const Bytes& bed)
    {
        if (bed.size() < 9)
            return;
        const uint8_t count = bed[1];
        if (count == 0)
            return;
        data["heatedBedTemperature"] = read_sacp_float(bed, 3);
        data["heatedBedTargetTemperature"] = read_i16_le(bed, 7);
    }

private:
    SacpEndpoint m_endpoint;
    std::string m_token;
    std::string m_error;
    bool m_connected { false };
    uint16_t m_sequence { 0 };
#ifndef _WIN32
    int m_fd { -1 };
#endif
};

} // namespace

SnapmakerJ1::SnapmakerJ1(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_token(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* SnapmakerJ1::get_name() const
{
    return "Snapmaker J1";
}

wxString SnapmakerJ1::get_test_ok_msg() const
{
    return _(L("Connected to Snapmaker J1 successfully."));
}

wxString SnapmakerJ1::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Snapmaker J1"), msg.Truncate(256));
}

bool SnapmakerJ1::test(wxString& curl_msg) const
{
    return connect_to_printer(&curl_msg);
}

bool SnapmakerJ1::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    wxString msg;
    const auto upload_filename = upload_data.upload_path.filename().empty() ? upload_data.source_path.filename() : upload_data.upload_path.filename();

    SacpTcpClient sacp(m_host, m_token);
    if (sacp.connect(&msg)) {
        if (info_fn)
            info_fn(_L("Uploading"), GUI::from_u8(sacp.url_label()));

        std::string md5;
        if (!sacp.upload_file(upload_data.source_path, upload_filename.string(), progress_fn, &msg, &md5)) {
            sacp.close();
            error_fn(get_test_failed_msg(msg));
            return false;
        }

        if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
            SacpResponse start_response = sacp.start_screen_print(upload_filename.string(), md5);
            if (!start_response.success || start_response.result != 0) {
                sacp.close();
                msg = GUI::from_u8(start_response.error.empty() ? sacp_result_message(start_response.result) : start_response.error);
                error_fn(get_test_failed_msg(msg));
                return false;
            }
        }

        sacp.close();
        return true;
    }

    BOOST_LOG_TRIVIAL(warning) << get_name() << ": SACP upload connection failed, falling back to HTTP: " << msg;
    if (!connect_to_printer(&msg)) {
        error_fn(get_test_failed_msg(msg));
        return false;
    }

    const auto url = make_url("api/v1/prepare_print");
    if (info_fn)
        info_fn(_L("Uploading"), GUI::from_u8(url));

    bool result = true;
    auto http = Http::post(url);
    apply_http_options(http, m_cafile, m_ssl_revoke_best_effort);

    http.form_add("token", m_token)
        .form_add("type", "3DP")
        .form_add_file("file", upload_data.source_path, upload_filename.string())
        .timeout_max(300)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << get_name() << ": prepare_print HTTP " << status << ": " << body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << get_name() << ": prepare_print failed, HTTP " << status << ": " << error << ", body: " << body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            if (progress_fn)
                progress_fn(std::move(progress), cancel);
            if (cancel)
                result = false;
        })
        .perform_sync();

    if (!result)
        return false;

    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        nlohmann::json start_response;
        if (!post_form("api/v1/start_print", { { "token", m_token } }, start_response, &msg, 120)) {
            error_fn(get_test_failed_msg(msg));
            return false;
        }
    }

    return true;
}

bool SnapmakerJ1::send_gcodes(const std::vector<std::string>& codes, std::string& extra_info)
{
    wxString msg;
    SacpTcpClient sacp(m_host, m_token);
    if (sacp.connect(&msg)) {
        nlohmann::json responses = nlohmann::json::array();
        for (const std::string& code : codes) {
            SacpResponse response = sacp.execute_gcode(code);
            responses.push_back(sacp_response_json(response));
            if (!response.success || response.result != 0) {
                extra_info = response.error.empty() ? sacp_result_message(response.result) : response.error;
                sacp.close();
                return false;
            }
        }
        sacp.close();
        extra_info = responses.dump();
        return true;
    }

    nlohmann::json responses = nlohmann::json::array();
    for (const std::string& code : codes) {
        nlohmann::json response;
        wxString msg;
        if (!post_form("api/v1/execute_code", { { "token", m_token }, { "code", code } }, response, &msg, 300)) {
            extra_info = msg.ToUTF8().data();
            return false;
        }
        responses.push_back(response);
    }

    extra_info = responses.dump();
    return true;
}

void SnapmakerJ1::async_get_printer_info(std::function<void(const nlohmann::json& response)> callback)
{
    async_get_device_info(std::move(callback));
}

void SnapmakerJ1::async_get_device_info(std::function<void(const nlohmann::json& response)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, callback = std::move(callback)]() mutable {
        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            nlohmann::json response = sacp.get_device_status();
            sacp.close();
            if (callback)
                callback(response);
            return;
        }

        const std::string query = token.empty() ? "api/v1/status" : "api/v1/status?token=" + Http::url_encode(token);
        auto result = perform_get_json(host, cafile, ssl_revoke_best_effort, query, 10);
        if (callback)
            callback(to_callback_json(result));
    }).detach();
}

void SnapmakerJ1::async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, scripts, callback = std::move(callback)]() mutable {
        nlohmann::json result;
        result["success"] = true;
        result["responses"] = nlohmann::json::array();

        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            result["sacp"] = true;
            for (const auto& script : scripts) {
                SacpResponse response = sacp.execute_gcode(script);
                result["responses"].push_back(sacp_response_json(response));
                if (!response.success || response.result != 0)
                    result["success"] = false;
            }
            sacp.close();
            if (callback)
                callback(result);
            return;
        }

        for (const auto& script : scripts) {
            auto response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/execute_code",
                                              { { "token", token }, { "code", script } }, 300);
            result["responses"].push_back(to_callback_json(response));
            if (!response.success)
                result["success"] = false;
        }

        if (callback)
            callback(result);
    }).detach();
}

void SnapmakerJ1::async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> callback)
{
    (void) filename;
    run_async_post(m_host, m_cafile, m_ssl_revoke_best_effort, "api/v1/start_print", { { "token", m_token } }, std::move(callback), 120);
}

void SnapmakerJ1::async_pause_print_job(std::function<void(const nlohmann::json&)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, callback = std::move(callback)]() mutable {
        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            SacpResponse response = sacp.pause_print();
            sacp.close();
            if (callback)
                callback(sacp_response_json(response));
            return;
        }
        auto response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/pause_print", { { "token", token } }, 120);
        if (callback)
            callback(to_callback_json(response));
    }).detach();
}

void SnapmakerJ1::async_resume_print_job(std::function<void(const nlohmann::json&)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, callback = std::move(callback)]() mutable {
        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            SacpResponse response = sacp.resume_print();
            sacp.close();
            if (callback)
                callback(sacp_response_json(response));
            return;
        }
        auto response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/resume_print", { { "token", token } }, 120);
        if (callback)
            callback(to_callback_json(response));
    }).detach();
}

void SnapmakerJ1::async_cancel_print_job(std::function<void(const nlohmann::json&)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, callback = std::move(callback)]() mutable {
        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            SacpResponse response = sacp.stop_print();
            sacp.close();
            if (callback)
                callback(sacp_response_json(response));
            return;
        }
        auto response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/stop_print", { { "token", token } }, 120);
        if (callback)
            callback(to_callback_json(response));
    }).detach();
}

void SnapmakerJ1::async_control_bed_temp(int temp, std::function<void(const nlohmann::json& response)> callback)
{
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, temp, callback = std::move(callback)]() mutable {
        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            SacpResponse response = sacp.set_bed_temperature(temp);
            sacp.close();
            if (callback)
                callback(sacp_response_json(response));
            return;
        }
        auto response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/override_bed_temperature",
                                          { { "token", token }, { "heatedBedTemp", std::to_string(temp) } }, 30);
        if (callback)
            callback(to_callback_json(response));
    }).detach();
}

void SnapmakerJ1::async_control_extruder_temp(int temp, int index, int map, std::function<void(const nlohmann::json& response)> callback)
{
    (void) map;
    const std::string host = m_host;
    const std::string token = m_token;
    const std::string cafile = m_cafile;
    const bool ssl_revoke_best_effort = m_ssl_revoke_best_effort;
    std::thread([host, token, cafile, ssl_revoke_best_effort, temp, index, callback = std::move(callback)]() mutable {
        nlohmann::json result;
        result["success"] = true;

        wxString msg;
        SacpTcpClient sacp(host, token);
        if (sacp.connect(&msg)) {
            SacpResponse response = sacp.set_extruder_temperature(temp, index);
            sacp.close();
            if (callback)
                callback(sacp_response_json(response));
            return;
        }

        auto switch_response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/switch_extruder",
                                                 { { "token", token }, { "active", std::to_string(index) } }, 30);
        result["switch_extruder"] = to_callback_json(switch_response);
        if (!switch_response.success)
            result["success"] = false;

        auto temp_response = perform_post_form(host, cafile, ssl_revoke_best_effort, "api/v1/override_nozzle_temperature",
                                               { { "token", token }, { "nozzleTemp", std::to_string(temp) } }, 30);
        result["set_temperature"] = to_callback_json(temp_response);
        if (!temp_response.success)
            result["success"] = false;

        if (callback)
            callback(result);
    }).detach();
}

std::string SnapmakerJ1::make_url(const std::string& path) const
{
    return make_url_from_host(m_host, path);
}

bool SnapmakerJ1::connect_to_printer(wxString* msg) const
{
    wxString sacp_msg;
    SacpTcpClient sacp(m_host, m_token);
    if (sacp.connect(&sacp_msg)) {
        sacp.close();
        return true;
    }

    nlohmann::json response;
    wxString http_msg;
    if (!post_form("api/v1/connect", { { "token", m_token } }, response, &http_msg, 3)) {
        if (msg)
            *msg = GUI::format_wxstr("SACP 8888: %s; HTTP 8080: %s", sacp_msg, http_msg);
        return false;
    }

    std::string token = get_string_if_exists(response, "token");
    if (token.empty() && response.contains("data"))
        token = get_string_if_exists(response["data"], "token");
    if (!token.empty())
        m_token = token;

    return true;
}

bool SnapmakerJ1::post_form(const std::string& path, const std::vector<std::pair<std::string, std::string>>& fields, nlohmann::json& response,
                            wxString* msg, long timeout) const
{
    const auto result = perform_post_form(m_host, m_cafile, m_ssl_revoke_best_effort, path, fields, timeout);
    response = result.json;
    if (result.success)
        return true;

    if (msg)
        *msg = format_error(result.body, result.error, result.status);
    return false;
}

bool SnapmakerJ1::get_json(const std::string& path, nlohmann::json& response, wxString* msg, long timeout) const
{
    const auto result = perform_get_json(m_host, m_cafile, m_ssl_revoke_best_effort, path, timeout);
    response = result.json;
    if (result.success)
        return true;

    if (msg)
        *msg = format_error(result.body, result.error, result.status);
    return false;
}

} // namespace Slic3r
