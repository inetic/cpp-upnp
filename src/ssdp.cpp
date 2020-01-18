#include <upnp/ssdp.h>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <queue>
#include <set>

#include "condition_variable.h"
#include "str/consume_until.h"
#include "str/consume_endpoint.h"
#include "str/istarts_with.h"
#include "str/trim.h"

namespace upnp { namespace ssdp {

struct query::state_t : std::enable_shared_from_this<state_t> {
    net::executor _exec;
    net::ip::udp::socket _socket;
    net::steady_timer _timer;
    ConditionVariable _cv;
    const net::ip::udp::endpoint _multicast_ep;
    std::queue<query::response> _responses;
    std::set<std::string> _already_seen_usns;

    optional<error_code> _stop_ec;
    optional<error_code> _rx_ec;

    state_t(net::executor exec)
        : _exec(std::move(exec))
        , _socket(_exec, net::ip::udp::v4())
        , _timer(_exec)
        , _cv(_exec)
        // https://www.grc.com/port_1900.htm
        , _multicast_ep(net::ip::address_v4({239, 255, 255, 250}), 1900)
    {}

    result<query::response> get_response(net::yield_context yield);

    result<void> start(net::yield_context);

    void stop(error_code);
};

result<void> query::state_t::start(net::yield_context yield)
{
    using udp = net::ip::udp;
    using namespace std::chrono;

    _socket.set_option(udp::socket::reuse_address(true));
    _socket.set_option(net::ip::multicast::join_group(_multicast_ep.address()));

    sys::error_code ec;
    _socket.bind(udp::endpoint(net::ip::address_v4::any(), 0), ec);
    if (ec) return ec;

    std::stringstream ss;

    seconds timeout(2);

    std::array<const char*, 2> search_targets
        = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1"
          , "urn:schemas-upnp-org:device:InternetGatewayDevice:2"
          //, "urn:schemas-upnp-org:service:WANIPConnection:1"
          //, "urn:schemas-upnp-org:service:WANIPConnection:2"
          //, "urn:schemas-upnp-org:service:WANPPPConnection:1"
          //, "urn:schemas-upnp-org:service:WANPPPConnection:2"
          //, "upnp:rootdevice"
          };

    for (auto target : search_targets) {
        // Section 1.3.2 in
        // http://upnp.org/specs/arch/UPnP-arch-DeviceArchitecture-v1.1.pdf
        ss << "M-SEARCH * HTTP/1.1\r\n"
           << "HOST: " << _multicast_ep << "\r\n"
           << "ST: " << target << "\r\n"
           << "MAN: \"ssdp:discover\"\r\n"
           << "MX: " << timeout.count() << "\r\n"
           << "USER-AGENT: asio-upnp/1.0\r\n"
           << "\r\n";

        auto sss = ss.str();

        _socket.async_send_to( net::buffer(sss.data(), sss.size())
                             , _multicast_ep
                             , yield[ec]);

        if (ec) return ec;
    }


    net::spawn(_exec, [&, self = shared_from_this()] (auto y) {
        _timer.expires_after(timeout + milliseconds(200));
        _timer.async_wait([&, self] (error_code) {
            if (_rx_ec) return;
            _rx_ec = net::error::timed_out;
            error_code ignored_ec;
            _socket.close(ignored_ec);
        });

        while (true) {
            std::array<char, 32*1024> rx;
            net::ip::udp::endpoint ep;
            net::mutable_buffer b(rx.data(), rx.size());
            sys::error_code ec;
            size_t size = _socket.async_receive_from(b, ep, y[ec]);

            if (!_rx_ec) {
                if (_stop_ec) _rx_ec = _stop_ec;
                else if (ec)  _rx_ec = ec;
            }
            if (_rx_ec) break;

            auto sv = string_view(rx.data(), size);
            auto r = response::parse(sv);
            if (!r) continue;
            // Don't add duplicates
            if (!_already_seen_usns.insert(r.value().usn).second) continue;
            _responses.push(std::move(r.value()));
            _cv.notify();
        }
        _cv.notify();
    });

    return success();
}

result<query::response> query::state_t::get_response(net::yield_context yield)
{
    auto self = shared_from_this();

    using namespace net::error;

    if (_stop_ec) {
        return *_stop_ec;
    }

    while (_responses.empty()) {
        if (_rx_ec) return *_rx_ec;
        sys::error_code ec;
        _cv.wait(yield[ec]);
    }

    auto r = std::move(_responses.front());
    _responses.pop();
    return std::move(r);
}

/* static */
result<query::response> query::response::parse(string_view lines)
{
    size_t line_n = 0;

    response ret;

    while (auto opt_line = str::consume_until(lines, {"\r\n", "\n"})) {
        auto line = *opt_line;

        if (line_n++ == 0) {
            if (!str::istarts_with(line, "http")) {
                return std::move(ret);
            }
            str::consume_until(line, " ");
            str::trim_space_prefix(line);
            auto result = str::consume_until(line, " ");
            if (!result || *result != "200") {
                return boost::system::errc::invalid_argument;
            }
            continue;
        }

        auto opt_key = str::consume_until(line, ":");
        if (!opt_key) break;

        auto key = *opt_key;
        auto val = line;
    
        str::trim_space_prefix(val);
        str::trim_space_suffix(val);

        if (boost::iequals(key, "USN")) {
            ret.usn = val.to_string();
            // USN: Unique Service Name
            // https://tools.ietf.org/html/draft-cai-ssdp-v1-03#section-2.2.2
            while (auto opt_token = str::consume_until(val, ":")) {
                if (boost::iequals(*opt_token, "uuid")) {
                    auto opt_uuid = str::consume_until(val, "::");
                    if (opt_uuid) {
                        ret.uuid = opt_uuid->to_string();
                    } else {
                        ret.uuid = val.to_string();
                    }
                }
            }
        } 
        if (boost::iequals(key, "LOCATION")) {
            auto location = url_t::parse(val.to_string());
            if (!location) {
                return sys::errc::invalid_argument;
            }
            ret.location = std::move(*location);
        }
        if (boost::iequals(key, "ST")) {
            // Service Type
            ret.service_type = val.to_string();
        }
    }

    return std::move(ret);
}

void query::state_t::stop(error_code ec) {
    _stop_ec = ec;
    _socket.close(ec);
    _timer.cancel();
}

query::query(std::shared_ptr<state_t> state)
    : _state(std::move(state))
{}

/* static */
result<query> query::start(net::executor exec, net::yield_context yield)
{
    auto st = std::make_shared<state_t>(exec);
    auto r = st->start(yield);
    if (!r) return r.error();
    return query{std::move(st)};
}

result<query::response> query::get_response(net::yield_context yield)
{
    return _state->get_response(yield);
}

void query::stop() {
    _state->stop(net::error::operation_aborted);
    _state = nullptr;
}

query::~query() {
    if (_state) stop();
}

}} // namespaces
