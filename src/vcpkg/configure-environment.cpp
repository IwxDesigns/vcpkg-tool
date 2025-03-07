#include <vcpkg/base/fwd/message_sinks.h>

#include <vcpkg/base/basic-checks.h>
#include <vcpkg/base/downloads.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/setup-messages.h>
#include <vcpkg/base/system.debug.h>
#include <vcpkg/base/system.process.h>

#include <vcpkg/archives.h>
#include <vcpkg/commands.version.h>
#include <vcpkg/configure-environment.h>
#include <vcpkg/metrics.h>
#include <vcpkg/tools.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>

#if defined(VCPKG_CE_SHA)
#define VCPKG_CE_SHA_AS_STRING MACRO_TO_STRING(VCPKG_CE_SHA)
#endif // ^^^ VCPKG_CE_SHA
#include <vcpkg/base/uuid.h>

namespace
{
    using namespace vcpkg;
#if !defined(VCPKG_ARTIFACTS_PATH)
    void extract_ce_tarball(const VcpkgPaths& paths,
                            const Path& ce_tarball,
                            const Path& node_path,
                            const Path& ce_base_path)
    {
        auto& fs = paths.get_filesystem();
        if (!fs.is_regular_file(ce_tarball))
        {
            Debug::println("Download succeeded but file isn't present?");
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgFailedToProvisionCe);
        }

        fs.remove_all(ce_base_path, VCPKG_LINE_INFO);
        fs.create_directories(ce_base_path, VCPKG_LINE_INFO);
        Path node_root = node_path.parent_path();
        auto npm_path = node_root / "node_modules" / "npm" / "bin" / "npm-cli.js";
        if (!fs.exists(npm_path, VCPKG_LINE_INFO))
        {
            npm_path = Path(node_root.parent_path()) / "lib" / "node_modules" / "npm" / "bin" / "npm-cli.js";
        }

        Command cmd_provision(node_path);
        cmd_provision.string_arg(npm_path);
        cmd_provision.string_arg("--force");
        cmd_provision.string_arg("install");
        cmd_provision.string_arg("--no-save");
        cmd_provision.string_arg("--no-lockfile");
        cmd_provision.string_arg("--scripts-prepend-node-path=true");
        cmd_provision.string_arg("--silent");
        cmd_provision.string_arg(ce_tarball);
        auto env = get_modified_clean_environment({}, node_root);
        const auto provision_status = cmd_execute(cmd_provision, WorkingDirectory{ce_base_path}, env);
        fs.remove(ce_tarball, VCPKG_LINE_INFO);
        if (!succeeded(provision_status))
        {
            fs.remove_all(ce_base_path, VCPKG_LINE_INFO);
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgFailedToProvisionCe);
        }
    }
#endif // ^^^ !defined(VCPKG_ARTIFACTS_PATH)

    void track_telemetry(Filesystem& fs, const Path& telemetry_file_path)
    {
        std::error_code ec;
        auto telemetry_file = fs.read_contents(telemetry_file_path, ec);
        if (ec)
        {
            Debug::println("Telemetry file couldn't be read: " + ec.message());
            return;
        }

        auto maybe_parsed = Json::parse_object(telemetry_file, telemetry_file_path);
        auto pparsed = maybe_parsed.get();

        if (!pparsed)
        {
            Debug::println("Telemetry file couldn't be parsed: " + maybe_parsed.error().data());
            return;
        }

        auto acquired_artifacts = pparsed->get("acquired_artifacts");
        if (acquired_artifacts)
        {
            if (acquired_artifacts->is_string())
            {
                get_global_metrics_collector().track_string(StringMetric::AcquiredArtifacts,
                                                            acquired_artifacts->string(VCPKG_LINE_INFO));
            }
            Debug::println("Acquired artifacts was not a string.");
        }
        else
        {
            Debug::println("No artifacts acquired.");
        }

        auto activated_artifacts = pparsed->get("activated_artifacts");
        if (activated_artifacts)
        {
            if (activated_artifacts->is_string())
            {
                get_global_metrics_collector().track_string(StringMetric::ActivatedArtifacts,
                                                            activated_artifacts->string(VCPKG_LINE_INFO));
            }
            Debug::println("Activated artifacts was not a string.");
        }
        else
        {
            Debug::println("No artifacts activated.");
        }
    }
}

namespace vcpkg
{
    int run_configure_environment_command(const VcpkgPaths& paths, View<std::string> args)
    {
        msg::println_warning(msgVcpkgCeIsExperimental);
        auto& fs = paths.get_filesystem();
        auto& download_manager = paths.get_download_manager();
        auto node_path = paths.get_tool_exe(Tools::NODE, stdout_sink);
#if defined(VCPKG_ARTIFACTS_PATH)
        // use hard coded in-source copy
        (void)fs;
        (void)download_manager;
        Path ce_path = MACRO_TO_STRING(VCPKG_ARTIFACTS_PATH);
        // development support: intentionally unlocalized
        msg::println(Color::warning,
                     LocalizedString::from_raw("Using in-development vcpkg-artifacts built at: ").append_raw(ce_path));
#else // ^^^ VCPKG_ARTIFACTS_PATH / might need to download vvv
#if defined(VCPKG_CE_SHA)
        auto base_path =
            get_platform_cache_vcpkg().value_or_exit(VCPKG_LINE_INFO) / "artifacts-" VCPKG_BASE_VERSION_AS_STRING;
        Debug::println("vcpkg-artifacts base path: ", base_path);
        auto ce_path = base_path / "node_modules" / "vcpkg-ce";
        bool needs_provisioning = !fs.is_directory(ce_path);
        if (needs_provisioning)
        {
            msg::println(msgDownloadingVcpkgCeBundle, msg::version = VCPKG_BASE_VERSION_AS_STRING);
            const auto ce_uri =
                "https://github.com/microsoft/vcpkg-tool/releases/download/" VCPKG_BASE_VERSION_AS_STRING
                "/vcpkg-ce.tgz";
            const auto ce_tarball = paths.downloads / "vcpkg-ce-" VCPKG_BASE_VERSION_AS_STRING ".tgz";
            download_manager.download_file(fs, ce_uri, {}, ce_tarball, VCPKG_CE_SHA_AS_STRING, null_sink);
            extract_ce_tarball(paths, ce_tarball, node_path, base_path);
        }
#else  // ^^^ VCPKG_CE_SHA (official build) // always get latest vvv
        auto base_path = get_platform_cache_vcpkg().value_or_exit(VCPKG_LINE_INFO) / "artifacts-latest";
        Debug::println("vcpkg-artifacts base path: ", base_path);
        auto ce_path = base_path / "node_modules" / "vcpkg-ce";
        msg::println(Color::warning, msgDownloadingVcpkgCeBundleLatest);
        const auto ce_uri = "https://github.com/microsoft/vcpkg-tool/releases/latest/download/vcpkg-ce.tgz";
        const auto ce_tarball = paths.downloads / "vcpkg-ce-latest.tgz";
        download_manager.download_file(fs, ce_uri, {}, ce_tarball, nullopt, null_sink);
        extract_ce_tarball(paths, ce_tarball, node_path, base_path);
#endif // ^^^ always get latest
#endif // ^^^ might need to download

        auto temp_directory = fs.create_or_get_temp_directory(VCPKG_LINE_INFO);

        Command cmd_run(node_path);
        cmd_run.string_arg(ce_path);
        cmd_run.forwarded_args(args);
        if (Debug::g_debugging)
        {
            cmd_run.string_arg("--debug");
        }

        Optional<Path> maybe_telemetry_file_path;
        if (g_metrics_enabled.load())
        {
            auto& p = maybe_telemetry_file_path.emplace(temp_directory /
                                                        (generate_random_UUID() + "_artifacts_telemetry.txt"));
            cmd_run.string_arg("--z-telemetry-file").string_arg(p);
        }

        cmd_run.string_arg("--vcpkg-root").string_arg(paths.root);
        cmd_run.string_arg("--z-vcpkg-command").string_arg(get_exe_path_of_current_process());

        cmd_run.string_arg("--z-vcpkg-artifacts-root").string_arg(paths.artifacts());
        cmd_run.string_arg("--z-vcpkg-downloads").string_arg(paths.downloads);
        cmd_run.string_arg("--z-vcpkg-registries-cache").string_arg(paths.registries_cache());
        cmd_run.string_arg("--z-next-previous-environment")
            .string_arg(temp_directory / (generate_random_UUID() + "_previous_environment.txt"));
        cmd_run.string_arg("--z-global-config").string_arg(paths.global_config());

        auto maybe_file = msg::get_loaded_file();
        if (!maybe_file.empty())
        {
            auto temp_file = temp_directory / "messages.json";
            fs.write_contents(temp_file, maybe_file, VCPKG_LINE_INFO);
            cmd_run.string_arg("--language").string_arg(temp_file);
        }

        auto result = cmd_execute(cmd_run, WorkingDirectory{paths.original_cwd}).value_or_exit(VCPKG_LINE_INFO);
        if (auto telemetry_file_path = maybe_telemetry_file_path.get())
        {
            track_telemetry(fs, *telemetry_file_path);
        }

        return result;
    }

    int run_configure_environment_command(const VcpkgPaths& paths, StringView arg0, View<std::string> args)
    {
        std::vector<std::string> all_args;
        all_args.reserve(args.size() + 1);
        all_args.emplace_back(arg0.data(), arg0.size());
        all_args.insert(all_args.end(), args.begin(), args.end());
        return run_configure_environment_command(paths, all_args);
    }
}
