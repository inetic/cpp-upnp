#include "xml.h"
#include "str/parse_address.h"
#include "str/consume_until.h"

namespace upnp { namespace xml {

using tree = boost::property_tree::ptree;

optional<tree> parse(const std::string& xml_str) {
    try {
        namespace ios = boost::iostreams;
        namespace pt = boost::property_tree;
        // https://stackoverflow.com/a/37712933/273348
        ios::stream<ios::array_source> stream(xml_str.c_str(), xml_str.size());
        pt::ptree tree;
        pt::read_xml(stream, tree);
        return std::move(tree);
    } catch (std::exception& e) {
        return none;
    }
}

optional<net::ip::address>
get_address(tree t, const char* tag) {
    auto s = t.get_optional<std::string>(tag);
    if (!s) return none;
    return str::parse_address(*s);
}

// This does the same as ptree::get_child_optional, but allows us to ignore
// the XML namespace prefix (e.g. the 's' in <s:Envelope/>) by using
// '*' in the query instead of the namespace name. e.g.:
//
// optional<string> os = get<string>(tree, "*:Envelope.*:Body");
const tree*
get_child(const tree& tr, string_view path)
{
    const tree* t = &tr;

    static const auto name_split = [] (string_view s)
        -> std::pair<string_view, string_view>
    {
        auto o = str::consume_until(s, ":");
        if (!o) return std::make_pair("", s);
        return std::make_pair(*o, s);
    };

    while (t) {
        if (path.empty()) return t;

        auto op = str::consume_until(path, ".");
        string_view p;
        if (op) { p = *op; } else { p = path; path = ""; }
        auto ns = name_split(p);

        auto t_ = t;
        t = nullptr;

        if (ns.first == "*") {
            for (auto& e : *t_) {
                auto name = e.first;
                auto ns_ = name_split(name);
                if (ns.second == ns_.second) {
                    t = &e.second;
                    break;
                }
            }
        } else {
            for (auto& e : *t_) {
                if (e.first == p) {
                    t = &e.second;
                    break;
                }
            }
        }
    }

    return t;
}

}} // namespace upnp::xml

namespace boost { namespace property_tree {

inline
std::ostream& operator<<(std::ostream& os, const ptree& xml) {
    write_xml(os, xml);
    return os;
}

}} // boost::property_tree namespace
