#ifndef slic3r_SnapmakerJ1_hpp_
#define slic3r_SnapmakerJ1_hpp_

#include <functional>
#include <string>
#include <vector>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;

class SnapmakerJ1 : public PrintHost
{
public:
    explicit SnapmakerJ1(DynamicPrintConfig* config);
    ~SnapmakerJ1() override = default;

    const char* get_name() const override;
    bool can_test() const override { return true; }
    std::string get_host() const override { return m_host; }
    bool has_auto_discovery() const override { return false; }

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    bool test(wxString& curl_msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    bool send_gcodes(const std::vector<std::string>& codes, std::string& extra_info) override;

    void async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) override;
    void async_get_device_info(std::function<void(const nlohmann::json& response)> callback) override;
    void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)> callback) override;
    void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> callback) override;
    void async_pause_print_job(std::function<void(const nlohmann::json&)> callback) override;
    void async_resume_print_job(std::function<void(const nlohmann::json&)> callback) override;
    void async_cancel_print_job(std::function<void(const nlohmann::json&)> callback) override;
    void async_control_bed_temp(int temp, std::function<void(const nlohmann::json& response)> callback) override;
    void async_control_extruder_temp(int temp, int index, int map, std::function<void(const nlohmann::json& response)> callback) override;

private:
    std::string m_host;
    mutable std::string m_token;
    std::string m_cafile;
    bool m_ssl_revoke_best_effort;

    std::string make_url(const std::string& path) const;
    bool connect_to_printer(wxString* msg = nullptr) const;
    bool post_form(const std::string& path, const std::vector<std::pair<std::string, std::string>>& fields, nlohmann::json& response,
                   wxString* msg = nullptr, long timeout = 120) const;
    bool get_json(const std::string& path, nlohmann::json& response, wxString* msg = nullptr, long timeout = 30) const;
};

} // namespace Slic3r

#endif
