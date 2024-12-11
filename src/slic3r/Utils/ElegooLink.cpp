#include "ElegooLink.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <curl/curl.h>

#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

    namespace {
    #ifdef WIN32
    std::string get_host_from_url(const std::string& url_in)
    {
        std::string url = url_in;
        // add http:// if there is no scheme
        size_t double_slash = url.find("//");
        if (double_slash == std::string::npos)
            url = "http://" + url;
        std::string out = url;
        CURLU* hurl = curl_url();
        if (hurl) {
            // Parse the input URL.
            CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Replace the address.
                char* host;
                rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
                if (rc == CURLUE_OK) {
                    char* port;
                    rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                    if (rc == CURLUE_OK && port != nullptr) {
                        out = std::string(host) + ":" + port;
                        curl_free(port);
                    } else {
                        out = host;
                        curl_free(host);
                    }
                }
                else
                    BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to get host form URL " << url;
            }
            else
                BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to parse URL " << url;
            curl_url_cleanup(hurl);
        }
        else
            BOOST_LOG_TRIVIAL(error) << "OctoPrint get_host_from_url: failed to allocate curl_url";
        return out;
    }

        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
    {
        // put ipv6 into [] brackets 
        if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[')
            sub_addr = "[" + sub_addr + "]";

    #if 0
        //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
        std::string final_addr = orig_addr;
        //  http
        size_t double_dash = orig_addr.find("//");
        size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
        // userinfo
        size_t at = orig_addr.find("@");
        host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
        // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
        // or it will be ']' if address is ipv6 )
        size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
        // if there are more ':' it must be ipv6
        if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
            size_t ipv6_end = orig_addr.find(']', host_start);
            // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
            potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
        }
        size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
        // now host_start and host_end should mark where to put resolved addr
        // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
        if (host_start >= orig_addr.length()) {
            return final_addr;
        }
        final_addr.replace(host_start, host_end - host_start, sub_addr);
        return final_addr;
    #else
        // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
        // If anything fails, return the input unchanged.
        std::string out = orig_addr;
        CURLU *hurl = curl_url();
        if (hurl) {
            // Parse the input URL.
            CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Replace the address.
                rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
                if (rc == CURLUE_OK) {
                    // Extract a string fromt the CURL URL handle.
                    char *url;
                    rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                    if (rc == CURLUE_OK) {
                        out = url;
                        curl_free(url);
                    } else
                        BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to extract the URL after substitution";
                } else
                    BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to substitute host " << sub_addr << " in URL " << orig_addr;
            } else
                BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to parse URL " << orig_addr;
            curl_url_cleanup(hurl);
        } else
            BOOST_LOG_TRIVIAL(error) << "OctoPrint substitute_host: failed to allocate curl_url";
        return out;
    #endif
    }
    #endif // WIN32
    std::string escape_string(const std::string& unescaped)
    {
        std::string ret_val;
        CURL* curl = curl_easy_init();
        if (curl) {
            char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
            if (decoded) {
                ret_val = std::string(decoded);
                curl_free(decoded);
            }
            curl_easy_cleanup(curl);
        }
        return ret_val;
    }
    std::string escape_path_by_element(const boost::filesystem::path& path)
    {
        std::string ret_val = escape_string(path.filename().string());
        boost::filesystem::path parent(path.parent_path());
        while (!parent.empty() && parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
        {
            ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
            parent = parent.parent_path();
        }
        return ret_val;
    }
} //namespace


    ElegooLink::ElegooLink(DynamicPrintConfig *config):
    OctoPrint(config) {

    }

    const char* ElegooLink::get_name() const { return "Elegoo Link"; }

    bool ElegooLink::elegoo_test(wxString& msg) const{

    const char *name = get_name();
    bool res = true;
    auto url = make_url("");
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            // Check if the response contains "ELEGOO" in any case.
            // This is a simple check to see if the response is from an Elegoo Link server.
            std::regex re("ELEGOO", std::regex::icase);
            std::smatch match;
            if (std::regex_search(body, match, re)) {
                res = true;
            } else {
                msg = format_error(body, "Elegoo Link not detected", 0);
                res = false;
            }
        })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
            .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
    }
    bool ElegooLink::test(wxString &curl_msg) const{
        if(OctoPrint::test(curl_msg)){
            return true;
        }
        curl_msg="";
        return elegoo_test(curl_msg);
    }
    wxString ElegooLink::get_test_ok_msg() const
    {
        return _(L("Connection to Elegoo Link works correctly."));
    }

    wxString ElegooLink::get_test_failed_msg(wxString& msg) const
    {
        return GUI::format_wxstr("%s: %s", _L("Could not connect to Elegoo Link"), msg);
    }

    #ifdef WIN32
    bool ElegooLink::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
    {
        info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

        // If test fails, test_msg_or_host_ip contains the error message.
        // Otherwise on Windows it contains the resolved IP address of the host.
        // Test_msg already contains resolved ip and will be cleared on start of test().
        wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());

        //Elegoo supports both otcoprint and Elegoo link
        if(OctoPrint::test_with_resolved_ip(test_msg_or_host_ip)){
            return OctoPrint::upload_inner_with_host(upload_data, prorgess_fn, error_fn, info_fn);
        }
        test_msg_or_host_ip="";
        if(!elegoo_test_with_resolved_ip(test_msg_or_host_ip)){
            error_fn(std::move(test_msg_or_host_ip));
            return false;
        }

        const char* name = get_name();
        const auto upload_filename = upload_data.upload_path.filename();
        const auto upload_parent_path = upload_data.upload_path.parent_path();
        //calc file size
        std::ifstream file(upload_data.source_path.string(), std::ios::binary | std::ios::ate);
        std::streamsize size = file.tellg();
        file.close();
        const std::string fileSize = std::to_string(size);
        // 创建一个随机UUID生成器
        boost::uuids::random_generator generator;
        // 生成一个UUID
        boost::uuids::uuid uuid = generator();
        // 将UUID转换为字符串
        std::string uuid_string = to_string(uuid);
        std::string md5;
        bbl_calc_md5(upload_data.source_path.string(), md5);

        std::string url = substitute_host(make_url("uploadFile/upload"), resolved_addr.to_string());
        bool result = true;

        info_fn(L"resolve", boost::nowide::widen(url));

        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%, uuid: %7%, fileSize: %8%, md5: %9%")
            % name
            % upload_data.source_path
            % url
            % upload_filename.string()
            % upload_parent_path.string()
            % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
            % uuid_string
            % fileSize
            % md5;

        std::string host = get_host_from_url(m_host);
        auto http = Http::post(url);//std::move(url));
        // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
        // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
        // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
        // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
        // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
        http.header("Host", host);
        set_auth(http);
        http.form_add("S-File-MD5", md5)
            .form_add("Check", "1")
            .form_add("Offset", "0")
            .form_add("Uuid",   uuid_string)
            .form_add("TotalSize",fileSize)
            .form_add_file("File", upload_data.source_path.string(), upload_filename.string())
            .on_complete([&](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                if(status == 200){
                    pt::ptree root;
                    std::istringstream is(body);
                    pt::read_json(is, root);
                    std::string code = root.get<std::string>("code");
                    if(code == "000000"){
                        // info_fn(L"complete", wxString());
                    }else{
                        //get error messages
                        pt::ptree messages = root.get_child("messages");
                        std::string error_message="ErrorCode : " + code + "\n";
                        for (pt::ptree::value_type &message : messages) {
                            std::string field = message.second.get<std::string>("field");
                            std::string msg = message.second.get<std::string>("message");
                            error_message += field + ":" + msg + "\n";
                        }
                        error_fn(wxString::FromUTF8(error_message));
                        result = false;
                    }
                }else{
                    error_fn(format_error(body, "upload failed", status));
                    result = false;
                }
            })
            .on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
                error_fn(format_error(body, error, status));
                result = false;
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                // If upload is finished, do not call progress_fn
                // on_complete will be called after some time, so we do not need to call it here
                // Because some devices will call on_complete after the upload progress reaches 100%, 
                //so we need to process it here, based on on_complete
                if(progress.ultotal == progress.ulnow){
                    // Upload is finished
                    return;
                }
                prorgess_fn(std::move(progress), cancel);
                if (cancel) {
                    // Upload was canceled
                    BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                    result = false;
                }
            })
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
            .perform_sync();

        return result;
    }
    #endif //WIN32

    bool ElegooLink::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
    {
        const char* name = get_name();

        const auto upload_filename = upload_data.upload_path.filename();
        const auto upload_parent_path = upload_data.upload_path.parent_path();
        std::string source_path = upload_data.source_path.string();
        //calc file size
        std::ifstream file(upload_data.source_path.string(), std::ios::binary | std::ios::ate);
        std::streamsize size = file.tellg();
        file.close();
        const std::string fileSize = std::to_string(size);
        // 创建一个随机UUID生成器
        boost::uuids::random_generator generator;
        // 生成一个UUID
        boost::uuids::uuid uuid = generator();
        // 将UUID转换为字符串
        std::string uuid_string = to_string(uuid);
        std::string md5;

        bbl_calc_md5(source_path, md5);

        // If test fails, test_msg_or_host_ip contains the error message.
        // Otherwise on Windows it contains the resolved IP address of the host.
        wxString test_msg_or_host_ip;
        //Elegoo supports both otcoprint and Elegoo link
        if(OctoPrint::test(test_msg_or_host_ip)){
            return OctoPrint::upload_inner_with_host(upload_data, prorgess_fn, error_fn, info_fn);
        }
        test_msg_or_host_ip="";
        if(!elegoo_test(test_msg_or_host_ip)){
            error_fn(std::move(test_msg_or_host_ip));
            return false;
        }

        std::string url;
        bool res = true;
    #ifdef WIN32
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
    #endif // _WIN32
        {
            // If https is entered we assume signed ceritificate is being used
            // IP resolving will not happen - it could resolve into address not being specified in cert
            url = make_url("uploadFile/upload");
        }
    #ifdef WIN32
        else {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Curl uses easy_getinfo to get ip address of last successful transaction.
            // If it got the address use it instead of the stored in "host" variable.
            // This new address returns in "test_msg_or_host_ip" variable.
            // Solves troubles of uploades failing with name address.
            // in original address (m_host) replace host for resolved ip 
            info_fn(L"resolve", test_msg_or_host_ip);
            url = substitute_host(make_url("uploadFile/upload"), GUI::into_u8(test_msg_or_host_ip));
            BOOST_LOG_TRIVIAL(info) << "Upload address after ip resolve: " << url;
        }
    #endif // _WIN32

        BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%, uuid: %7%, fileSize: %8%, md5: %9%")
            % name
            % upload_data.source_path
            % url
            % upload_filename.string()
            % upload_parent_path.string()
            % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
            % uuid_string
            % fileSize
            % md5;

        auto http = Http::post(std::move(url));
    #ifdef WIN32
        // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted it into "url" variable.
        // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
        // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
        // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
        // Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
        // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
        std::string host = get_host_from_url(m_host);
        http.header("Host", host);
    #endif // _WIN32
        set_auth(http);
        http.form_add("Check", "1")
            .form_add("S-File-MD5", md5)
            .form_add("Offset", "0")
            .form_add("Uuid", uuid_string)
            .form_add("TotalSize", fileSize)
            .form_add_file("File", upload_data.source_path.string(), upload_filename.string())
            .on_complete([&](std::string body, unsigned status) {
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                if (status == 200) {
                    pt::ptree root;
                    std::istringstream is(body);
                    pt::read_json(is, root);
                    std::string code = root.get<std::string>("code");
                    if (code == "000000") {
                        // info_fn(L"complete", wxString());
                    } else {
                        //get error messages
                        pt::ptree messages = root.get_child("messages");
                        std::string error_message = "ErrorCode : " + code + "\n";
                        for (pt::ptree::value_type& message : messages) {
                            std::string field = message.second.get<std::string>("field");
                            std::string msg = message.second.get<std::string>("message");
                            error_message += field + ":" + msg + "\n";
                        }
                        error_fn(wxString::FromUTF8(error_message));
                        res = false;
                    }
                } else {
                    error_fn(format_error(body, "upload failed", status));
                    res = false;
                }
            })
            .on_error([&](std::string body, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
                error_fn(format_error(body, error, status));
                res = false;
            })
            .on_progress([&](Http::Progress progress, bool& cancel) {
                // If upload is finished, do not call progress_fn
                // on_complete will be called after some time, so we do not need to call it here
                // Because some devices will call on_complete after the upload progress reaches 100%, 
                //so we need to process it here, based on on_complete
                if(progress.ultotal == progress.ulnow){
                    // Upload is finished
                    return;
                }
                prorgess_fn(std::move(progress), cancel);
                if (cancel) {
                    // Upload was canceled
                    BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                    res = false;
                }
            })
    #ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
    #endif
            .perform_sync();

        return res;
    }

    bool ElegooLink::validate_version_text(const boost::optional<std::string> &version_text) const
    {
        return version_text ? boost::starts_with(*version_text, "OctoPrint") : true;
    }

    bool ElegooLink::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
    {
    #ifndef WIN32
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    #else
        std::string host = get_host_from_url(m_host);

        // decide what to do based on m_host - resolve hostname or upload to ip
        std::vector<boost::asio::ip::address> resolved_addr;
        boost::system::error_code ec;
        boost::asio::ip::address host_ip = boost::asio::ip::make_address(host, ec);
        if (!ec) {
            resolved_addr.push_back(host_ip);
        } else if ( GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")){
            Bonjour("octoprint")
                .set_hostname(host)
                .set_retries(5) // number of rounds of queries send
                .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
                .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply>& replies) {
                    for (const auto & rpl : replies) {
                        boost::asio::ip::address ip(rpl.ip);
                        ra.emplace_back(ip);
                        BOOST_LOG_TRIVIAL(info) << "Resolved IP address: " << rpl.ip;
                    }
                })
                .resolve_sync();
        }
        if (resolved_addr.empty()) {
            // no resolved addresses - try system resolving
            BOOST_LOG_TRIVIAL(error) << "ElegooLink failed to resolve hostname " << m_host << " into the IP address. Starting upload with system resolving.";
            return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
        } else if (resolved_addr.size() == 1) {
            // one address resolved - upload there
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
        }  else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
            // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
            // try sending to both. (Then if both fail, show both error msg after second try)
            wxString error_message;
            if (!upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
                , [&msg = error_message, resolved_addr](wxString error) { msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); }
                , info_fn, resolved_addr.front())
                &&
                !upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn
                , [&msg = error_message, resolved_addr](wxString error) { msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error); }
                , info_fn, resolved_addr.back())
                ) {

                error_fn(error_message);
                return false;
            }
            return true;
        } else {
            // There are multiple addresses - user needs to choose which to use.
            size_t selected_index = resolved_addr.size(); 
            IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
            if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {    
                return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
            }
        }
        return false;
    #endif // WIN32
    }


    #ifdef WIN32

    bool ElegooLink::elegoo_test_with_resolved_ip(wxString& msg) const{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool res = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url(""), GUI::into_u8(msg));
    msg.Clear();
    std::string host = get_host_from_url(m_host);
    auto http = Http::get(url);//std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734).
    // Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version at %2% : %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            // Check if the response contains "ELEGOO" in any case.
            // This is a simple check to see if the response is from an Elegoo Link server.
            std::regex re("ELEGOO", std::regex::icase);
            std::smatch match;
            if (std::regex_search(body, match, re)) {
                res = true;
            } else {
                msg = format_error(body, "Elegoo Link not detected", 0);
                res = false;
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return res;
    }
    bool ElegooLink::test_with_resolved_ip(wxString &msg) const
    {
        //Elegoo supports both otcoprint and Elegoo link
        if(OctoPrint::test_with_resolved_ip(msg)){
            return true;
        }
        msg="";
        return elegoo_test_with_resolved_ip(msg);
    }
    #endif //WIN32
}
