#include <vector>
#include "environ.h"

#include <optional>
#include <string>

extern "C" {
#include <slurm/slurm_errno.h>
#include <slurm/spank.h>
}

#include <uenv/env.h>
#include <uenv/parse.h>
#include <uenv/telemetry.h>

#include "environ.h"
#include "plugin.h"

namespace uenv::slurm {

// wrapper spank_getenv : for use in the remote context
std::optional<std::string> getenv_wrapper(spank_t sp, const char* var) {
    // UENV_MOUNT_LIST has one entry of some 150 characters for each layer:
    // a fixed buffer of 1024 bytes holds only 6 layers. Grow the buffer
    // while spank_getenv reports that the value does not fit.
    for (std::size_t len = 1024; len <= (1u << 20); len *= 4) {
        std::vector<char> buf(len);
        const auto ret = spank_getenv(sp, var, buf.data(), int(len));
        if (ret == ESPANK_ENV_NOEXIST) {
            return std::nullopt;
        }
        if (ret == ESPANK_SUCCESS) {
            return std::string{buf.data()};
        }
        if (ret != ESPANK_NOSPACE) {
            slurm_spank_log("getenv failed");
            throw ret;
        }
    }
    slurm_spank_log("getenv failed: the value of %s is too long", var);
    throw ESPANK_NOSPACE;
}

bool in_slurm_uenv_session(const envvars::state& env) {
    return uenv::in_uenv_session(env) && env.get("SLURM_UENV");
}

// Update the calling environment to apply the environment variable updates.
// note: making this into a nice clean function that returns state that can be
// used by the calling slurm plugin was too hard - because slurm requires that
// the use of setenv/getenv/unsetenv or spank_getenv/spank_setenv/spank_unsetenv
void patch_slurm_environment(const envvars::state& full_env,
                             envvars::state const& base, const arg_pack& args) {
    // set each variable whose value changed, and unset each variable that
    // the session removed
    for (auto& [name, value] : full_env.variables()) {
        auto old = base.get(name);
        if (!old || *old != value) {
            setenv(name.c_str(), value.c_str(), 1);
        }
    }
    for (auto& [name, _] : base.variables()) {
        if (!full_env.get(name)) {
            unsetenv(name.c_str());
        }
    }

    // unset the SBATCH_UENV* environment variables, so that they can't
    // affect calls to srun and sbatch inside the called script/jobstep
    for (const auto name :
         {"SBATCH_UENV", "SBATCH_UENV_VIEW", "SBATCH_UENV_REPO"}) {
        if (full_env.get(name)) {
            ::unsetenv(name);
        }
    }

    std::string view_v =
        full_env.get("UENV_VIEW").value_or(args.view_description.value_or(""));
    std::string repo_v =
        full_env.get("UENV_REPO").value_or(args.repo_description.value_or(""));
    std::string uenv_v =
        full_env.get("UENV_LABEL").value_or(args.uenv_description.value_or(""));
    ::setenv("SLURM_UENV", uenv_v.c_str(), 1);
    ::setenv("SLURM_UENV_VIEW", view_v.c_str(), 1);
    ::setenv("SLURM_UENV_REPO", repo_v.c_str(), 1);
}

} // namespace uenv::slurm
