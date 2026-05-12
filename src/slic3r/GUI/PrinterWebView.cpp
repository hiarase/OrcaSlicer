#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "common_func/common_func.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/msgdlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include "slic3r/GUI/SSWCP.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

static const wxColour WEB_DEVICE_STATUS_BG(248, 248, 248);
static const wxColour WEB_DEVICE_STATUS_TEXT(48, 58, 60);
static const wxColour WEB_DEVICE_STATUS_MUTED(107, 107, 107);

static wxString format_temperature_pair(int current, int target)
{
    return wxString::Format("%d/%d C", current, target);
}

static wxString format_remaining_time(int seconds)
{
    if (seconds <= 0)
        return _L("N/A");
    return wxString::FromUTF8(get_bbl_monitor_time_dhm(seconds));
}

static bool json_as_double(const nlohmann::json& object, const char* key, double& value)
{
    if (!object.is_object() || !object.contains(key) || object[key].is_null())
        return false;

    const auto& node = object[key];
    if (node.is_number()) {
        value = node.get<double>();
        return true;
    }
    if (node.is_string()) {
        try {
            value = std::stod(node.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

static wxString json_as_wxstring(const nlohmann::json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_string())
        return wxEmptyString;
    return wxString::FromUTF8(object[key].get<std::string>());
}

static bool response_success(const nlohmann::json& response)
{
    if (!response.is_object())
        return false;
    if (response.contains("success") && response["success"].is_boolean())
        return response["success"].get<bool>();

    double status = 0.0;
    if (json_as_double(response, "http_status", status))
        return status >= 200.0 && status < 300.0;
    return false;
}

static const nlohmann::json* snapmaker_status_payload(const nlohmann::json& response)
{
    const nlohmann::json* payload = &response;
    if (payload->is_object() && payload->contains("data") && (*payload)["data"].is_object())
        payload = &(*payload)["data"];
    if (payload->is_object() && !payload->contains("status") && payload->contains("data") && (*payload)["data"].is_object())
        payload = &(*payload)["data"];
    return payload;
}

static bool snapmaker_status_can_stop(const wxString& status)
{
    const wxString lower = status.Lower();
    return lower.Contains("running") || lower.Contains("printing") || lower.Contains("paused") ||
           lower.Contains("pausing") || lower.Contains("resuming");
}

static wxString snapmaker_nozzle_status(const nlohmann::json& status)
{
    wxString text;
    auto append = [&status, &text](const wxString& label, const char* current_key, const char* target_key) {
        double current = 0.0;
        if (!json_as_double(status, current_key, current))
            return;

        double target = 0.0;
        json_as_double(status, target_key, target);
        if (!text.empty())
            text += "  ";
        text += label + " " + format_temperature_pair((int) std::lround(current), (int) std::lround(target));
    };

    append("T0", "nozzleTemperature1", "nozzleTargetTemperature1");
    append("T1", "nozzleTemperature2", "nozzleTargetTemperature2");
    if (text.empty()) {
        append("T0", "nozzleTemperature", "nozzleTargetTemperature");
        append("T1", "nozzleRightTemperature", "nozzleRightTargetTemperature");
    }
    return text.empty() ? _L("N/A") : text;
}

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    init_status_panel(topsizer);

    wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) + "/web/flutter_web/index.html?path=2");
    auto     real_url = wxGetApp().get_international_url(url);
      // Create the webview
    m_browser = WebView::CreateWebView(this, real_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
    Bind(wxEVT_TIMER, &PrinterWebView::on_status_timer, this);

    m_status_timer.SetOwner(this);
    m_status_timer.Start(1000);
    update_status_panel();

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    m_status_timer.Stop();
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}

void PrinterWebView::init_status_panel(wxBoxSizer *topsizer)
{
    m_status_panel = new wxPanel(this, wxID_ANY);
    m_status_panel->SetBackgroundColour(WEB_DEVICE_STATUS_BG);

    auto *status_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto add_metric = [this, status_sizer](const wxString& label, wxStaticText*& value) {
        auto *label_text = new wxStaticText(m_status_panel, wxID_ANY, label + ":");
        label_text->SetForegroundColour(WEB_DEVICE_STATUS_MUTED);
        value = new wxStaticText(m_status_panel, wxID_ANY, _L("N/A"));
        value->SetForegroundColour(WEB_DEVICE_STATUS_TEXT);
        status_sizer->Add(label_text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(14));
        status_sizer->Add(value, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    };

    m_machine_name = new wxStaticText(m_status_panel, wxID_ANY, _L("Device"));
    m_machine_name->SetForegroundColour(WEB_DEVICE_STATUS_TEXT);
    status_sizer->Add(m_machine_name, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(14));

    add_metric(_L("Progress"), m_progress_value);
    add_metric(_L("Time"), m_time_value);
    add_metric(_L("Bed"), m_bed_value);
    add_metric(_L("Nozzle"), m_nozzle_value);

    status_sizer->AddStretchSpacer();

    m_stop_button = new wxButton(m_status_panel, wxID_ANY, _L("Stop"), wxDefaultPosition, wxSize(FromDIP(88), FromDIP(28)));
    m_stop_button->SetToolTip(_L("Cancel print"));
    m_stop_button->Bind(wxEVT_BUTTON, &PrinterWebView::on_stop_print, this);
    status_sizer->Add(m_stop_button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));

    m_status_panel->SetSizer(status_sizer);
    topsizer->Add(m_status_panel, 0, wxEXPAND);
}

Slic3r::MachineObject* PrinterWebView::get_status_machine() const
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev)
        return nullptr;

    if (MachineObject *selected = dev->get_selected_machine())
        return selected;

    const std::string first_online = dev->get_first_online_user_machine();
    if (!first_online.empty()) {
        if (MachineObject *online = dev->get_my_machine(first_online))
            return online;
    }

    auto machines = dev->get_my_machine_list();
    return machines.empty() ? nullptr : machines.begin()->second;
}

void PrinterWebView::update_status_panel()
{
    if (has_direct_snapmaker_status()) {
        update_direct_status_panel();
        if (++m_direct_status_tick >= 2) {
            m_direct_status_tick = 0;
            request_direct_status();
        }
        return;
    }

    update_machine_status_panel();
}

void PrinterWebView::update_machine_status_panel()
{
    MachineObject *obj = get_status_machine();
    const bool has_obj = obj != nullptr;
    const bool connected = has_obj && obj->is_connected();
    const bool printing = connected && obj->is_in_printing();

    m_machine_name->SetLabel(has_obj ? wxString::FromUTF8(obj->dev_name.empty() ? obj->dev_id : obj->dev_name) : _L("Device"));
    m_progress_value->SetLabel(printing ? wxString::Format("%d%%", obj->mc_print_percent) : _L("N/A"));
    m_time_value->SetLabel(printing ? format_remaining_time(obj->mc_left_time) : _L("N/A"));
    m_bed_value->SetLabel(connected ? format_temperature_pair((int)obj->bed_temp, (int)obj->bed_temp_target) : _L("N/A"));

    wxString nozzle_text = _L("N/A");
    if (connected && !obj->m_extder_data.extders.empty()) {
        nozzle_text.clear();
        for (size_t i = 0; i < obj->m_extder_data.extders.size(); ++i) {
            const Extder& ext = obj->m_extder_data.extders[i];
            if (!nozzle_text.empty())
                nozzle_text += "  ";
            nozzle_text += wxString::Format("T%zu ", i) + format_temperature_pair(ext.temp, ext.target_temp);
        }
    }
    m_nozzle_value->SetLabel(nozzle_text);

    m_stop_button->Enable(connected && obj->can_abort());
    m_status_panel->Layout();
}

bool PrinterWebView::has_direct_snapmaker_status() const
{
    if (!wxGetApp().preset_bundle)
        return false;

    const DynamicPrintConfig& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto* host_type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return host_type && host_type->value == htSnapmakerJ1 && !config.opt_string("print_host").empty();
}

void PrinterWebView::request_direct_status()
{
    if (m_direct_status_busy || !has_direct_snapmaker_status())
        return;

    DynamicPrintConfig config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config, false));
    if (!host)
        return;

    m_direct_status_busy = true;
    host->async_get_device_info([this](const nlohmann::json& response) {
        wxGetApp().CallAfter([this, response]() {
            m_direct_status_busy = false;
            apply_direct_status_response(response);
            update_direct_status_panel();
        });
    });
}

void PrinterWebView::apply_direct_status_response(const nlohmann::json& response)
{
    const nlohmann::json* payload = snapmaker_status_payload(response);
    const bool has_payload = payload && payload->is_object() && !payload->empty();
    m_direct_status_online = response_success(response) && has_payload;
    if (!m_direct_status_online) {
        m_direct_status_printing = false;
        m_direct_progress = -1;
        m_direct_remaining_seconds = -1;
        m_direct_bed_text = _L("N/A");
        m_direct_nozzle_text = _L("N/A");
        return;
    }

    wxString machine_name = json_as_wxstring(*payload, "machineName");
    if (machine_name.empty())
        machine_name = json_as_wxstring(*payload, "name");
    if (machine_name.empty())
        machine_name = json_as_wxstring(*payload, "series");
    m_direct_machine_name = machine_name.empty() ? _L("Snapmaker J1") : machine_name;

    const wxString status = json_as_wxstring(*payload, "status");
    m_direct_status_printing = snapmaker_status_can_stop(status);

    double progress = -1.0;
    if (json_as_double(*payload, "progress", progress)) {
        if (progress <= 1.0)
            progress *= 100.0;
        m_direct_progress = std::clamp((int) std::lround(progress), 0, 100);
    } else {
        double current_line = 0.0;
        double total_lines = 0.0;
        if (json_as_double(*payload, "currentLine", current_line) && json_as_double(*payload, "totalLines", total_lines) && total_lines > 0.0)
            m_direct_progress = std::clamp((int) std::lround((current_line / total_lines) * 100.0), 0, 100);
        else
            m_direct_progress = -1;
    }

    double remaining = 0.0;
    m_direct_remaining_seconds = json_as_double(*payload, "remainingTime", remaining) ? (int) std::lround(remaining) : -1;

    double bed_current = 0.0;
    double bed_target = 0.0;
    if (json_as_double(*payload, "heatedBedTemperature", bed_current)) {
        json_as_double(*payload, "heatedBedTargetTemperature", bed_target);
        m_direct_bed_text = format_temperature_pair((int) std::lround(bed_current), (int) std::lround(bed_target));
    } else {
        m_direct_bed_text = _L("N/A");
    }

    m_direct_nozzle_text = snapmaker_nozzle_status(*payload);
}

void PrinterWebView::update_direct_status_panel()
{
    m_machine_name->SetLabel(m_direct_machine_name.empty() ? _L("Snapmaker J1") : m_direct_machine_name);
    m_progress_value->SetLabel(m_direct_status_online && m_direct_progress >= 0 ? wxString::Format("%d%%", m_direct_progress) : _L("N/A"));
    m_time_value->SetLabel(m_direct_status_online ? format_remaining_time(m_direct_remaining_seconds) : _L("N/A"));
    m_bed_value->SetLabel(m_direct_status_online ? m_direct_bed_text : _L("N/A"));
    m_nozzle_value->SetLabel(m_direct_status_online ? m_direct_nozzle_text : _L("N/A"));
    m_stop_button->Enable(m_direct_status_online && m_direct_status_printing && !m_direct_cancel_busy);
    m_status_panel->Layout();
}

void PrinterWebView::on_status_timer(wxTimerEvent& evt)
{
    (void) evt;
    update_status_panel();
}

void PrinterWebView::on_stop_print(wxCommandEvent& evt)
{
    (void) evt;
    if (has_direct_snapmaker_status()) {
        if (!m_direct_status_online || !m_direct_status_printing || m_direct_cancel_busy)
            return;

        const int answer = wxMessageBox(_L("Are you sure you want to cancel this print?"), _L("Cancel print"),
                                        wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (answer != wxYES)
            return;

        DynamicPrintConfig config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config, false));
        if (!host)
            return;

        m_direct_cancel_busy = true;
        update_direct_status_panel();
        host->async_cancel_print_job([this](const nlohmann::json& response) {
            wxGetApp().CallAfter([this, response]() {
                m_direct_cancel_busy = false;
                if (!response_success(response))
                    wxMessageBox(_L("Failed to cancel print."), _L("Cancel print"), wxOK | wxICON_ERROR, this);
                m_direct_status_tick = 1;
                request_direct_status();
                update_direct_status_panel();
            });
        });
        return;
    }

    MachineObject *obj = get_status_machine();
    if (!obj || !obj->is_connected() || !obj->can_abort())
        return;

    const int answer = wxMessageBox(_L("Are you sure you want to cancel this print?"), _L("Cancel print"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    if (answer == wxYES)
        obj->command_task_abort();
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;
    m_apikey_sent = false;
    
    if (url.find("path=2") != std::string::npos) {
        wxGetApp().fltviews().add_printer_view(this, url, apikey);
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
    }

    m_browser->Show();
    m_browser->LoadURL(url);

    UpdateState();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

bool PrinterWebView::isSnapmakerPage()
{
    auto url = m_browser->GetCurrentURL();
    return (url.find("flutter_web") != std::string::npos);
}

void PrinterWebView::sendMessage(const std::string& msg) {
    WebView::RunScript(m_browser, msg);
}

void PrinterWebView::update_mode()
{
    // m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    m_browser->EnableAccessToDevTools(true);
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    SendAPIKey();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
