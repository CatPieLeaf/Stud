#include "stud/album_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/trap_recovery.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <thread>

namespace stud::jni_bridge {

namespace {

using ImageSavedFn = void (*)(JNIEnv*, jclass, jstring, jboolean, jstring);

// What Android's MimeTypeMap would call it, reduced to the one question
// the Java side asks of the answer: is it a video. Everything else the
// engine hands over is a screenshot.
bool is_video(const std::filesystem::path& file) {
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".mov" || ext == ".3gp";
}

// `name` in `dir`, or the first "name (n).ext" that is free.
std::filesystem::path free_name_in(const std::filesystem::path& dir, const std::string& name) {
    std::filesystem::path candidate = dir / name;
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) return candidate;
    const std::filesystem::path p(name);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    for (int n = 2; n < 10000; ++n) {
        candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return dir / name;
}

void report(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib, const std::string& name,
            bool ok, const std::string& error) {
    auto* fn = reinterpret_cast<ImageSavedFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeImageSavedToAlbumFinished"));
    if (fn == nullptr) {
        std::printf("stud: album: the engine has no nativeImageSavedToAlbumFinished; it will not "
                    "hear that %s was saved\n",
                    name.c_str());
        std::fflush(stdout);
        return;
    }
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    call_trapping_abort(fn, jni_env, static_cast<jclass>(nullptr),
                        env.NewStringUTF(name.c_str()), static_cast<jboolean>(ok ? 1 : 0),
                        env.NewStringUTF(error.c_str()));
    clear_pending_jni_exception(jni_env, "nativeImageSavedToAlbumFinished");
}

}  // namespace

void save_capture_to_album(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                           const AlbumFolders& folders, const std::string& path) {
    // Off the calling thread, as Android runs it on its IO dispatcher: the
    // engine calls this from its own thread and a recording can be large.
    std::thread([&jvm, &lib, folders, path] {
        const std::filesystem::path source(path);
        const std::string name = source.filename().string();
        const bool video = is_video(source);
        const std::string& dir_name = video ? folders.videos : folders.pictures;
        bool ok = false;
        std::string error;
        std::filesystem::path destination;
        if (dir_name.empty()) {
            error = "no destination folder";
        } else {
            std::error_code ec;
            std::filesystem::create_directories(dir_name, ec);
            destination = free_name_in(dir_name, name);
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::none, ec);
            ok = !ec;
            if (ec) error = ec.message();
        }
        // The path is the user's own folder, not a secret; the name is the
        // engine's own timestamped file name.
        if (ok) {
            std::printf("stud: album: saved %s to %s\n", video ? "a recording" : "a screenshot",
                        destination.c_str());
        } else {
            std::printf("stud: album: could not save %s (%s)\n", name.c_str(), error.c_str());
        }
        std::fflush(stdout);
        report(jvm, lib, name, ok, error);
    }).detach();
}

}  // namespace stud::jni_bridge
