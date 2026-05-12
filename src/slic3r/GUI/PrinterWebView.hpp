#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>
#include <wx/button.h>
#include <wx/stattext.h>

#include "nlohmann/json_fwd.hpp"


namespace Slic3r {
class MachineObject;
namespace GUI {

class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    bool isSnapmakerPage();
    void sendMessage(const std::string& msg);
    wxWebView* get_browser() const { return m_browser; }

private:
    void SendAPIKey();
    void init_status_panel(wxBoxSizer *topsizer);
    void update_status_panel();
    void update_machine_status_panel();
    void update_direct_status_panel();
    void request_direct_status();
    void apply_direct_status_response(const nlohmann::json& response);
    void on_status_timer(wxTimerEvent& evt);
    void on_stop_print(wxCommandEvent& evt);
    bool has_direct_snapmaker_status() const;
    Slic3r::MachineObject* get_status_machine() const;

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    bool m_apikey_sent;
    wxTimer m_status_timer;
    wxPanel* m_status_panel { nullptr };
    wxStaticText* m_machine_name { nullptr };
    wxStaticText* m_progress_value { nullptr };
    wxStaticText* m_time_value { nullptr };
    wxStaticText* m_bed_value { nullptr };
    wxStaticText* m_nozzle_value { nullptr };
    wxButton* m_stop_button { nullptr };
    bool m_direct_status_busy { false };
    bool m_direct_cancel_busy { false };
    bool m_direct_status_online { false };
    bool m_direct_status_printing { false };
    int m_direct_status_tick { 1 };
    int m_direct_progress { -1 };
    int m_direct_remaining_seconds { -1 };
    wxString m_direct_machine_name;
    wxString m_direct_bed_text;
    wxString m_direct_nozzle_text;

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
