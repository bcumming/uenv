// vim: ts=4 sts=4 sw=4 et

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <spdlog/spdlog.h>

#include <uenv/env.h>
#include <uenv/parse.h>
#include <uenv/session.h>
#include <uenv/telemetry.h>
#include <util/strings.h>

#include "help.h"
#include "status.h"
#include "terminal.h"

namespace uenv {

namespace {

enum class status_format { name, full, views };

struct status_args {
    status_format format = status_format::full;
    // return nonzero value when no uenv is loaded
    bool error_if_unset = false;
};

std::string status_footer();

int status(const status_args& args, const global_settings& settings);

} // namespace

argparse::command status_command(const global_settings& settings) {
    argparse::command_builder<status_args> status_cli(
        "status", "print information about the currently loaded uenv");
    status_cli.add_flag("error-if-unset", &status_args::error_if_unset,
                        "return a nonzero error code if no uenv is loaded");
    status_cli.action([&settings](const status_args& args) {
        return status(args, settings);
    });
    status_cli.add_choice("format", &status_args::format,
                          {{"short", status_format::name},
                           {"full", status_format::full},
                           {"views", status_format::views}},
                          "one of {full (default), short, views}");

    status_cli.footer(status_footer);

    return std::move(status_cli).build();
}

namespace {

// print the items of UENV_SESSION (1-composition.md, "Concretisation")
void print_session(const std::vector<session_item>& items, status_format f) {
    using enum status_format;
    auto views_of = [](const session_item& d) {
        std::vector<std::string> v;
        for (auto& x : d.views) {
            v.push_back(color::cyan(x.string()));
        }
        return v;
    };
    if (f == name) {
        std::vector<std::string> v;
        for (auto& d : items) {
            v.push_back(d.name.string());
        }
        term::msg("{}", fmt::join(v, ","));
        return;
    }
    if (f == views) {
        std::vector<std::string> v;
        for (auto& d : items) {
            v.push_back(d.views.empty()
                            ? color::yellow(d.name.string())
                            : fmt::format("{}[{}]", color::yellow(d.name.string()),
                                          fmt::join(views_of(d), ",")));
        }
        term::msg("{}", fmt::join(v, ","));
        return;
    }
    // the widths of the columns of the layers, over all items
    std::size_t wa = 0, wl = 0;
    for (auto& d : items) {
        for (auto& l : d.layers) {
            wa = std::max(wa, l.alias.string().size());
            wl = std::max(wl, l.label.size());
        }
    }
    for (auto& d : items) {
        const char* kind = d.kind == session_item::kind_t::layer ? "layer"
                           : d.kind == session_item::kind_t::file ? "file "
                                                                  : "image";
        term::msg("uenv   {}", color::yellow(d.name.string()));
        term::msg("  {}  {}", kind, color::cyan(d.label));
        term::msg("  views  [{}]", fmt::join(views_of(d), ", "));
        term::msg("  layers");
        for (auto& l : d.layers) {
            term::msg("    {}  {}  {}",
                      color::yellow(fmt::format("{:<{}}", l.alias.string(), wa)),
                      fmt::format("{:<{}}", l.label, wl),
                      color::cyan(l.mount.string()));
        }
    }
}

int status([[maybe_unused]] const status_args& args,
           [[maybe_unused]] const global_settings& settings) {
    spdlog::info("uenv status");
    using enum status_format;

    if (!in_uenv_session(settings.calling_environment)) {
        // only print output in full mode
        if (args.format == full) {
            term::msg("there is no uenv loaded");
        }
        return args.error_if_unset ? 1 : 0;
    }

    // the items of the session. A v1 session, or a session of an older uenv,
    // has no UENV_SESSION: then show the mounts of UENV_TELEMETRY
    if (auto v = settings.calling_environment.get("UENV_SESSION")) {
        if (auto items = parse_session_json(*v)) {
            print_session(*items, args.format);
            return 0;
        } else {
            spdlog::warn("{}: the mounts of UENV_TELEMETRY are shown",
                         items.error());
        }
    }

    auto telemetry = uenv::telemetry_from_env(settings.calling_environment);

    if (!telemetry) {
        term::error("unable to find uenv status");
        return 1;
    }

    if (args.format == full) {
        for (auto& E : *telemetry) {
            const std::string image = E.label ? E.label.value() : E.sqfs;
            term::msg(
                "uenv  {}\n  image  {}\n  mount  {}\n  views  [{}]",
                color::yellow(E.name), color::cyan(image), color::cyan(E.mount),
                fmt::join(E.views | std::views::transform([](const auto& v) {
                              return color::cyan(v);
                          }),
                          ", "));
        }
    } else if (args.format == views) {
        term::msg(
            "{}",
            fmt::join(*telemetry | std::views::transform([](const auto& u) {
                if (u.views.empty()) {
                    return fmt::format("{}", color::yellow(u.name));
                }
                return fmt::format(
                    "{}[{}]", color::yellow(u.name),
                    fmt::join(u.views |
                                  std::views::transform([](const auto& v) {
                                      return color::cyan(v);
                                  }),
                              ","));
            }),
                      ","));
    } else if (args.format == name) {
        term::msg("{}", fmt::join(*telemetry |
                                      std::views::transform(
                                          [](const auto& u) { return u.name; }),
                                  ","));
    }

    return 0;
}

std::string status_footer() {
    using enum help::block::admonition;
    std::vector<help::item> items{
        // clang-format off
        help::block{none, "Display information about the current uenv environment." },
        help::linebreak{},
        help::block{xmpl, "get status:"},
        help::block{code,   "uenv status"},
        help::linebreak{},
        help::block{note, "if no uenv is loaded, the message 'there is no no uenv loaded' will be printed"},
        help::block{xmpl, "control the formatting of the output:"},
        help::block{code,   "uenv status --format=full  # verbose output (default)"},
        help::block{code,   "uenv status --format=short # comma-separated list of uenv name"},
        help::block{code,   "uenv status --format=views # comma-separated list of uenv:[views]"},
        help::linebreak{},
        help::block{note, "the 'name' and 'views' options print no output if no uenv is running.",
                          "These options are useful for scripting and setting command line prompts."},
        // clang-format on
    };

    return fmt::format("{}", fmt::join(items, "\n"));
}

} // namespace

} // namespace uenv
