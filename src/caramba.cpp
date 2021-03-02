// Copyright (c) 2020, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <Rcpp.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>

#include <csignal>

#include "mamba/output.hpp"
#include "mamba/util.hpp"
#include "mamba/context.hpp"
#include "mamba/thread_utils.hpp"

#include "mamba/channel.hpp"
#include "mamba/subdirdata.hpp"
#include "mamba/pool.hpp"
#include "mamba/solver.hpp"
#include "mamba/package_cache.hpp"
#include "mamba/transaction.hpp"

const char banner[] = R"MAMBARAW(
                                           __
          __  ______ ___  ____ _____ ___  / /_  ____ _
         / / / / __ `__ \/ __ `/ __ `__ \/ __ \/ __ `/
        / /_/ / / / / / / /_/ / / / / / / /_/ / /_/ /
       / .___/_/ /_/ /_/\__,_/_/ /_/ /_/_.___/\__,_/
      /_/
)MAMBARAW";

namespace fs = ghc::filesystem;
namespace r = Rcpp;
using namespace mamba;

// [[Rcpp::plugins(cpp17)]]

/* Beginning of Context */
fs::path target_prefix = std::getenv("CONDA_PREFIX") ? std::getenv("CONDA_PREFIX") : "";;
// Need to prevent circular imports here (otherwise using env::get())
fs::path root_prefix = std::getenv("MAMBA_ROOT_PREFIX") ? std::getenv("MAMBA_ROOT_PREFIX") : "";
fs::path conda_prefix = root_prefix;

// TODO check writable and add other potential dirs
std::vector<fs::path> envs_dirs = { root_prefix / "envs" };
std::vector<fs::path> pkgs_dirs = { root_prefix / "pkgs" };

bool const MAMBA_USE_INDEX_CACHE = false;
std::size_t MAMBA_LOCAL_REPODATA_TTL = 1; // take from header
bool const MAMBA_OFFLINE = 0;
bool const MAMBA_QUIET = 1;
bool const MAMBA_JSON = 1;
bool const MAMBA_AUTO_ACTIVATE_BASE = false;

long MAMBA_MAX_PARALLEL_DOWNLOADS = 5;
int MAMBA_VERBOSITY = 0;

bool const MAMBA_DEV = false;
bool const MAMBA_ON_CI = false;
bool const MAMBA_NO_PROGRESS_BARS = false;
bool const MAMBA_DRY_RUN = false;
bool const MAMBA_ALWAYS_YES = false;

// // debug helpers
bool const MAMBA_KEEP_TEMP_FILES = false;
bool const MAMBA_KEEP_TEMP_DIRECTORIES = false;

bool const MAMBA_SIG_INTERRUPT = false;

bool const MAMBA_CHANGE_PS1 = true;

int MAMBA_CONNECT_TIMEOUT_SECS = 10;
int MAMBA_RETRY_TIMEOUT = 2; // seconds
int MAMBA_RETRY_BACKOFF = 3; // retry_timeout * retry_backoff
int MAMBA_MAX_RETRIES = 3; // max number of retries

// Conda compat
bool const MAMBA_ADD_PIP_AS_PYTHON_DEPENDENCY = true;
/* End of Context */

static struct {
    std::vector<std::string> specs;
    std::string prefix;
    std::string name;
    std::vector<std::string> channels;
} create_options;

static struct {
    bool ssl_verify = true;
    std::string cacert_path;
} network_options;

struct formatted_pkg
{
    std::string name, version, build, channel;
};

bool compareAlphabetically(const formatted_pkg& a, const formatted_pkg& b)
{
    return a.name < b.name;
}

// [[Rcpp::export]]
void set_conda_version(std::string conda_version)
{
    mamba::Context::instance().conda_version = conda_version;
}

// [[Rcpp::export]]
void set_verbosity(int lvl)
{
    mamba::Context::instance().verbosity = lvl;
}

// [[Rcpp::export]]
void set_root_prefix(std::string root_prefix)
{
    mamba::Context::instance().root_prefix = root_prefix;
}

// [[Rcpp::export]]
void set_channels(std::vector<std::string> channels)
{
    for (std::string channel : channels)
    {
        mamba::Context::instance().channels.push_back(channel);
    }
}

// [[Rcpp::export]]
void set_opt(int option, bool value)
{
    switch (option)
    {
        //MAMBA_USE_INDEX_CACHE
        case 1:
            mamba::Context::instance().use_index_cache = value;
            break;
        //MAMBA_OFFLINE
        case 2:
            mamba::Context::instance().offline = value;
            break;
        //MAMBA_QUIET
        case 3:
            mamba::Context::instance().quiet = value;
            break;
        //MAMBA_JSON
        case 4:
            mamba::Context::instance().json = value;
            break;
        //MAMBA_AUTO_ACTIVATE_BASE
        case 5:
            mamba::Context::instance().auto_activate_base = value;
            break;
        //MAMBA_DEV
        case 6:
            mamba::Context::instance().dev = value;
            break;
        //MAMBA_ON_CI
        case 7:
            mamba::Context::instance().on_ci = value;
            break;
        //MAMBA_NO_PROGRESS_BARS
        case 8:
            mamba::Context::instance().no_progress_bars = value;
            break;
        //MAMBA_DRY_RUN
        case 9:
            mamba::Context::instance().dry_run = value;
            break;
        //MAMBA_ALWAYS_YES
        case 10:
            mamba::Context::instance().always_yes = value;
            break;
        //MAMBA_KEEP_TEMP_FILES
        case 11:
            mamba::Context::instance().keep_temp_files = value;
            break;
        //MAMBA_KEEP_TEMP_DIRECTORIES
        case 12:
            mamba::Context::instance().keep_temp_directories = value;
            break;
        //MAMBA_CHANGE_PS1
        case 13:
            mamba::Context::instance().change_ps1 = value;
            break;
        //MAMBA_ADD_PIP_AS_PYTHON_DEPENDENCY
        case 14:
            mamba::Context::instance().add_pip_as_python_dependency = value;
            break;
    }
}

// [[Rcpp::export]]
void print_context()
{
    for (auto& channel : Context::instance().channels)
    {
        Rcpp::Rcout << "Channel: " << channel << std::endl;
    }
    Rcpp::Rcout << "Root prefix: " << mamba::Context::instance().root_prefix << "\n";
    Rcpp::Rcout << "Target prefix: " << mamba::Context::instance().target_prefix << "\n";
    Rcpp::Rcout << "Use index cache: " << Context::instance().use_index_cache << "\n";
    Rcpp::Rcout << "Is offline: " << Context::instance().offline << "\n";
    Rcpp::Rcout << "Verbosity level: " << Context::instance().verbosity << "\n";
    Rcpp::Rcout << "Is quiet: " << Context::instance().quiet << "\n";
    Rcpp::Rcout << "Is json: " << Context::instance().json << "\n";
    Rcpp::Rcout << "Auto activate base: " << Context::instance().auto_activate_base << "\n";
    Rcpp::Rcout << "Is dev: " << Context::instance().dev << "\n";
    Rcpp::Rcout << "Is on CI: " << Context::instance().on_ci << "\n";
    Rcpp::Rcout << "Is no no progress bars: " << Context::instance().no_progress_bars << "\n";
    Rcpp::Rcout << "Is dry run: " << Context::instance().dry_run << "\n";
    Rcpp::Rcout << "Always yes: " << Context::instance().quiet << "\n";
    Rcpp::Rcout << "Keep temporary files: " << Context::instance().keep_temp_files << "\n";
    Rcpp::Rcout << "Keep temporary directories: " << Context::instance().keep_temp_directories << "\n";
    Rcpp::Rcout << "Change PS1: " << Context::instance().change_ps1 << "\n";
    Rcpp::Rcout << "Add pip as python dependency: " << Context::instance().add_pip_as_python_dependency << "\n";
}

void set_global_options(Context& ctx)
{
    ctx.verbosity = mamba::Context::instance().verbosity;
    ctx.quiet = mamba::Context::instance().quiet;
    ctx.json = mamba::Context::instance().json;
    ctx.always_yes = mamba::Context::instance().always_yes;
    ctx.offline = mamba::Context::instance().offline;
    ctx.dry_run = mamba::Context::instance().dry_run;
}

void set_network_options(Context& ctx)
{
    std::array<std::string, 6> cert_locations {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian/Ubuntu/Gentoo etc.
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora/RHEL 6
        "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
        "/etc/pki/tls/cacert.pem",                           // OpenELEC
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS/RHEL 7
        "/etc/ssl/cert.pem",                                 // Alpine Linux
    };

    // ssl verify can be either an empty string (regular SSL verification),
    // the string "<false>" to indicate no SSL verification, or a path to
    // a directory with cert files, or a cert file.
    if (network_options.ssl_verify == false)
    {
        ctx.ssl_verify = "<false>";
    }
    else if (!network_options.cacert_path.empty())
    {
        ctx.ssl_verify = network_options.cacert_path;
    }
    else
    {
        for (const auto& loc : cert_locations)
        {
            if (fs::exists(loc))
            {
                ctx.ssl_verify = loc;
            }
        }
        if (ctx.ssl_verify.empty())
        {
            LOG_WARNING << "No ca certificates found, disabling SSL verification";
            ctx.ssl_verify = "<false>";
        }
    }
}

void install_specs(const std::vector<std::string>& specs, bool create_env = false)
{
    auto& ctx = Context::instance();

    set_global_options(ctx);

    Console::print(banner);

    if (ctx.root_prefix.empty())
    {
        Rcpp::Rcout << "You have not set a $MAMBA_ROOT_PREFIX.\nEither set the MAMBA_ROOT_PREFIX \
        environment variable, or use `mamba::set_root_prefix` or use\n  micromamba shell init ... \
        \nto initialize your shell, then restart or source the contents of the shell init script.\n";
        exit(1);
    }

    if (ctx.target_prefix.empty())
    {
        Rcpp::Rcout << "No active target prefix.\n\nRun $ micromamba activate \
        <PATH_TO_MY_ENV>\nto activate an environment.\n";
        exit(1);
    }
    if (!fs::exists(ctx.target_prefix) && create_env == false)
    {
        Rcpp::Rcout << "Prefix does not exist\n";
        exit(1);
    }

    fs::path cache_dir = ctx.root_prefix / "pkgs" / "cache";
    try
    {
        fs::create_directories(cache_dir);
    }
    catch (...)
    {
        Rcpp::Rcout << "Could not create `pkgs/cache/` dirs" << std::endl;
        exit(1);
    }

    std::vector<std::string> channel_urls = calculate_channel_urls(ctx.channels);

    std::vector<std::shared_ptr<MSubdirData>> subdirs;
    MultiDownloadTarget multi_dl;

    for (auto& url : channel_urls)
    {
        auto& channel = make_channel(url);
        std::string full_url = concat(channel.url(true), "/repodata.json");

        auto sdir = std::make_shared<MSubdirData>(
            concat(channel.name(), "/", channel.platform()),
            full_url,
            cache_dir / cache_fn_url(full_url)
        );

        sdir->load();
        multi_dl.add(sdir->target());
        subdirs.push_back(sdir);
    }
    multi_dl.download(true);

    std::vector<MRepo> repos;
    MPool pool;
    PrefixData prefix_data(ctx.target_prefix);
    prefix_data.load();
    auto repo = MRepo(pool, prefix_data);
    repos.push_back(repo);

    int prio_counter = subdirs.size();
    for (auto& subdir : subdirs)
    {
        MRepo repo = subdir->create_repo(pool);
        repo.set_priority(prio_counter--, 0);
        repos.push_back(repo);
    }

    MSolver solver(pool, { { SOLVER_FLAG_ALLOW_DOWNGRADE, 1 } });
    solver.add_jobs(specs, SOLVER_INSTALL);
    solver.solve();

    mamba::MultiPackageCache package_caches({ctx.root_prefix / "pkgs"});
    mamba::MTransaction trans(solver, package_caches);

    if (ctx.json)
    {
        trans.log_json();
    }
    // TODO this is not so great
    std::vector<MRepo*> repo_ptrs;
    for (auto& r : repos) { repo_ptrs.push_back(&r); }

    bool yes = trans.prompt(ctx.root_prefix / "pkgs", repo_ptrs);
    if (!yes) exit(0);

    if (create_env && !Context::instance().dry_run)
    {
        fs::create_directories(ctx.target_prefix);
        fs::create_directories(ctx.target_prefix / "conda-meta");
        fs::create_directories(ctx.target_prefix / "pkgs");
    }

    trans.execute(prefix_data, ctx.root_prefix / "pkgs");
}

// [[Rcpp::export]]
void list()
{
    auto& ctx = Context::instance();
    PrefixData prefix_data(ctx.target_prefix);
    prefix_data.load();

    Rcpp::Rcout << "List of packages in environment: " << ctx.target_prefix << std::endl;

    formatted_pkg formatted_pkgs;

    std::vector<formatted_pkg> packages;

    // order list of packages from prefix_data by alphabetical order
    for (auto package : prefix_data.m_package_records)
    {
        formatted_pkgs.name = package.second.name;
        formatted_pkgs.version = package.second.version;
        formatted_pkgs.build = package.second.build_string;
        if (package.second.channel.find("https://repo.anaconda.com/pkgs/") == 0)
        {
            formatted_pkgs.channel = "";
        }
        else
        {
            Channel& channel = make_channel(package.second.url);
            formatted_pkgs.channel = channel.name();
        }
        packages.push_back(formatted_pkgs);
    }

    std::sort(packages.begin(), packages.end(), compareAlphabetically);

    // format and print table
    printers::Table t({ "Name", "Version", "Build", "Channel" });
    t.set_alignment({ printers::alignment::left,
                      printers::alignment::left,
                      printers::alignment::left,
                      printers::alignment::left });
    t.set_padding({ 2, 2, 2, 2 });

    for (auto p : packages)
    {
        t.add_row({ p.name, p.version, p.build, p.channel });
    }

    t.print(Rcpp::Rcout);
}

// [[Rcpp::export]]
void install(const std::vector<std::string>& specs, bool create_env = false)
{
    std::unordered_set<std::string> channels_set(mamba::Context::instance().channels.begin(), mamba::Context::instance().channels.end());
    channels_set.insert({"default", "conda-forge"});

    mamba::Context::instance().channels.assign(channels_set.begin(), channels_set.end());

    set_network_options(mamba::Context::instance());
    install_specs(specs, create_env);
}
