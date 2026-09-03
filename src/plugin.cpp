#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"EasyRec2.Window";
constexpr wchar_t kWindowTitle[] = L"簡易録音2";
constexpr UINT WM_RECORDER_DONE = WM_APP + 44;
constexpr UINT WM_RECORDER_ERROR = WM_APP + 45;
constexpr UINT WM_RECORDER_READY = WM_APP + 46;
constexpr UINT_PTR TIMER_PLAYBACK_CHECK = 7001;
constexpr UINT_PTR TIMER_RECORDING_CLOCK = 7002;

enum ControlId : int {
    IDC_DEVICE = 1001,
    IDC_REFRESH,
    IDC_FORMAT,
    IDC_RATE,
    IDC_CHANNELS,
    IDC_QUALITY,
    IDC_FOLDER,
    IDC_BROWSE_FOLDER,
    IDC_FFMPEG,
    IDC_BROWSE_FFMPEG,
    IDC_AUTO_ADD,
    IDC_START,
    IDC_STOP,
    IDC_OPEN_FOLDER,
    IDC_STATUS,
    IDC_PLAY_PREVIEW,
    IDC_DURATION,
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct RecordSettings {
    std::wstring deviceId;
    std::wstring outputFolder;
    std::wstring ffmpegPath;
    int formatIndex = 0;
    int rate = 48000;
    int channels = 2;
    int qualityIndex = 1;
    bool autoAdd = true;
    bool playPreview = true;
    int targetLayer = 0;
    int targetFrame = 0;
};

struct RecordResult {
    bool ok = false;
    std::wstring path;
    std::wstring message;
    bool autoAdd = false;
    int layer = 0;
    int frame = 0;
    double durationSeconds = 0.0;
};

HINSTANCE g_module = nullptr;
HWND g_window = nullptr;
HWND g_hostWindow = nullptr;
EDIT_HANDLE* g_edit = nullptr;
LOG_HANDLE* g_logger = nullptr;
CONFIG_HANDLE* g_config = nullptr;
std::vector<DeviceInfo> g_devices;
std::wstring g_iniPath;
bool g_comInitialized = false;
bool g_previewStartedByPlugin = false;
bool g_previewStartPending = false;
bool g_previewTogglePosted = false;
int g_previewCheckAttempts = 0;
int g_recordStartLayer = 0;
int g_recordStartFrame = 0;
bool g_recordingClockStarted = false;
std::chrono::steady_clock::time_point g_recordingStartTime{};

std::wstring WinError(HRESULT hr) {
    wchar_t* raw = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring text = raw ? raw : L"不明なエラー";
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return std::format(L"{} (0x{:08X})", text, static_cast<unsigned>(hr));
}

void LogInfo(const std::wstring& text) {
    if (g_logger) g_logger->info(g_logger, text.c_str());
}

void LogError(const std::wstring& text) {
    if (g_logger) g_logger->error(g_logger, text.c_str());
}

std::wstring GetWindowTextString(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length + 1), L'\0');
    if (length > 0) GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void SetStatus(const std::wstring& value) {
    if (g_window) SetWindowTextW(GetDlgItem(g_window, IDC_STATUS), value.c_str());
}

std::wstring FormatRecordingTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const auto tenths = static_cast<unsigned long long>(seconds * 10.0);
    const auto hours = tenths / 36000;
    const auto minutes = (tenths / 600) % 60;
    const auto wholeSeconds = (tenths / 10) % 60;
    const auto fraction = tenths % 10;
    if (hours > 0) {
        return std::format(L"録音時間 {:02}:{:02}:{:02}.{}", hours, minutes, wholeSeconds, fraction);
    }
    return std::format(L"録音時間 {:02}:{:02}.{}", minutes, wholeSeconds, fraction);
}

void SetRecordingTime(double seconds) {
    if (g_window) SetWindowTextW(GetDlgItem(g_window, IDC_DURATION), FormatRecordingTime(seconds).c_str());
}

void StartRecordingClock() {
    if (!g_window) return;
    g_recordingStartTime = std::chrono::steady_clock::now();
    g_recordingClockStarted = true;
    SetRecordingTime(0.0);
    SetTimer(g_window, TIMER_RECORDING_CLOCK, 100, nullptr);
}

double StopRecordingClock() {
    if (g_window) KillTimer(g_window, TIMER_RECORDING_CLOCK);
    if (!g_recordingClockStarted) return 0.0;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_recordingStartTime).count();
    g_recordingClockStarted = false;
    SetRecordingTime(elapsed);
    return elapsed;
}

std::wstring ModuleDirectory() {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size);
    return fs::path(path).parent_path().wstring();
}

std::wstring FindFfmpeg() {
    fs::path local = fs::path(ModuleDirectory()) / L"ffmpeg.exe";
    if (fs::exists(local)) return local.wstring();
    wchar_t found[32768]{};
    DWORD size = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr);
    if (size > 0 && size < std::size(found)) return std::wstring(found, size);
    return {};
}

std::wstring TimestampName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format(L"AviUtl2_rec_{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

const wchar_t* ExtensionFor(int format) {
    switch (format) {
    case 0: return L".wav";
    case 1: return L".flac";
    case 2: return L".mp3";
    case 3: return L".m4a";
    case 4: return L".opus";
    default: return L".wav";
    }
}

const wchar_t* OutputMuxerFor(int format) {
    switch (format) {
    case 0: return L"wav";
    case 1: return L"flac";
    case 2: return L"mp3";
    case 3: return L"ipod";
    case 4: return L"opus";
    default: return L"wav";
    }
}

std::wstring UniqueOutputPath(const RecordSettings& settings) {
    fs::path base = fs::path(settings.outputFolder) / (TimestampName() + ExtensionFor(settings.formatIndex));
    if (!fs::exists(base)) return base.wstring();
    for (int i = 2; i < 10000; ++i) {
        fs::path candidate = base.parent_path() / (base.stem().wstring() + std::format(L"_{:02}", i) + base.extension().wstring());
        if (!fs::exists(candidate)) return candidate.wstring();
    }
    return base.wstring();
}

std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\"";
    unsigned backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring QualityArguments(int format, int index) {
    switch (format) {
    case 0: {
        static constexpr const wchar_t* codecs[] = {L"pcm_s16le", L"pcm_s24le", L"pcm_f32le"};
        return std::format(L"-c:a {}", codecs[(index >= 0 && index < 3) ? index : 1]);
    }
    case 1: {
        static constexpr int levels[] = {5, 8, 12};
        return std::format(L"-c:a flac -sample_fmt s32 -compression_level {}", levels[(index >= 0 && index < 3) ? index : 1]);
    }
    case 2: {
        static constexpr int rates[] = {128, 192, 256, 320};
        return std::format(L"-c:a libmp3lame -b:a {}k", rates[(index >= 0 && index < 4) ? index : 1]);
    }
    case 3: {
        static constexpr int rates[] = {128, 192, 256, 320};
        return std::format(L"-c:a aac -b:a {}k -movflags +faststart", rates[(index >= 0 && index < 4) ? index : 1]);
    }
    case 4: {
        static constexpr int rates[] = {64, 96, 128, 192, 256};
        return std::format(L"-c:a libopus -b:a {}k -vbr on", rates[(index >= 0 && index < 5) ? index : 2]);
    }
    default:
        return L"-c:a pcm_s16le";
    }
}

const wchar_t* RawFormatFor(const WAVEFORMATEX* format) {
    WORD tag = format->wFormatTag;
    WORD bits = format->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) return L"f32le";
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT) return L"f32le";
    if (tag == WAVE_FORMAT_PCM) {
        if (bits == 8) return L"u8";
        if (bits == 16) return L"s16le";
        if (bits == 24) return L"s24le";
        if (bits == 32) return L"s32le";
    }
    return nullptr;
}

class EncoderProcess {
public:
    ~EncoderProcess() { Close(); }

    bool Start(const RecordSettings& settings, const WAVEFORMATEX* source, const std::wstring& finalPath, std::wstring& error) {
        finalPath_ = finalPath;
        partialPath_ = finalPath + L".part";
        logPath_ = finalPath + L".ffmpeg.log";

        const wchar_t* raw = RawFormatFor(source);
        if (!raw) {
            error = L"録音デバイスの音声形式に対応していません。";
            return false;
        }
        if (settings.ffmpegPath.empty() || !fs::exists(settings.ffmpegPath)) {
            error = L"FFmpegが見つかりません。FFmpeg欄でffmpeg.exeを指定してください。";
            return false;
        }

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readPipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe_, &sa, 0)) {
            error = L"FFmpeg用の音声パイプを作成できませんでした。";
            return false;
        }
        SetHandleInformation(writePipe_, HANDLE_FLAG_INHERIT, 0);

        logFile_ = CreateFileW(logPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile_ == INVALID_HANDLE_VALUE) {
            logFile_ = nullptr;
            CloseHandle(readPipe);
            error = L"FFmpegログを作成できませんでした。";
            return false;
        }

        std::wstring command = Quote(settings.ffmpegPath) +
            std::format(L" -hide_banner -loglevel warning -nostdin -y -f {} -ar {} -ac {} -i pipe:0 -vn -ar {} -ac {} {} -f {} {}",
                raw, source->nSamplesPerSec, source->nChannels, settings.rate, settings.channels,
                QualityArguments(settings.formatIndex, settings.qualityIndex), OutputMuxerFor(settings.formatIndex), Quote(partialPath_));

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = readPipe;
        startup.hStdOutput = logFile_;
        startup.hStdError = logFile_;
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        BOOL ok = CreateProcessW(settings.ffmpegPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(readPipe);
        if (!ok) {
            error = L"FFmpegを起動できませんでした: " + WinError(HRESULT_FROM_WIN32(GetLastError()));
            Close();
            return false;
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        return true;
    }

    bool Write(const BYTE* data, DWORD bytes, std::wstring& error) {
        if (!writePipe_) return false;
        DWORD written = 0;
        if (!WriteFile(writePipe_, data, bytes, &written, nullptr) || written != bytes) {
            error = L"FFmpegへの音声転送に失敗しました。";
            return false;
        }
        return true;
    }

    bool Finish(std::wstring& error) {
        if (writePipe_) {
            CloseHandle(writePipe_);
            writePipe_ = nullptr;
        }
        if (!process_) return false;
        DWORD wait = WaitForSingleObject(process_, 15000);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 2000);
            error = L"FFmpegの終了を待てなかったため停止しました。";
            Close();
            return false;
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process_, &exitCode);
        CloseHandle(process_);
        process_ = nullptr;
        if (logFile_) {
            CloseHandle(logFile_);
            logFile_ = nullptr;
        }
        if (exitCode != 0 || !fs::exists(partialPath_)) {
            error = std::format(L"FFmpegがエラー終了しました (code {})。ログ: {}", exitCode, logPath_);
            return false;
        }
        std::error_code ec;
        fs::rename(partialPath_, finalPath_, ec);
        if (ec) {
            const std::string detail = ec.message();
            error = L"完成した録音ファイルの名前を確定できませんでした: " + std::wstring(detail.begin(), detail.end());
            return false;
        }
        DeleteFileW(logPath_.c_str());
        return true;
    }

    void Close() {
        if (writePipe_) { CloseHandle(writePipe_); writePipe_ = nullptr; }
        if (process_) { TerminateProcess(process_, 1); CloseHandle(process_); process_ = nullptr; }
        if (logFile_) { CloseHandle(logFile_); logFile_ = nullptr; }
    }

private:
    HANDLE writePipe_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE logFile_ = nullptr;
    std::wstring finalPath_;
    std::wstring partialPath_;
    std::wstring logPath_;
};

class Recorder {
public:
    ~Recorder() { Stop(); }

    bool Start(RecordSettings settings) {
        if (running_.exchange(true)) return false;
        if (worker_.joinable()) worker_.join();
        stopRequested_ = false;
        worker_ = std::thread([this, settings = std::move(settings)] { Run(settings); });
        return true;
    }

    void Stop() {
        RequestStop();
        if (worker_.joinable()) worker_.join();
    }

    void RequestStop() {
        stopRequested_ = true;
        HANDLE event = event_.load();
        if (event) SetEvent(event);
    }

    bool IsRunning() const { return running_; }

private:
    static std::wstring WaveError(MMRESULT code) {
        wchar_t buffer[MAXERRORLENGTH]{};
        if (waveInGetErrorTextW(code, buffer, static_cast<UINT>(std::size(buffer))) == MMSYSERR_NOERROR) return buffer;
        return std::format(L"waveIn error {}", code);
    }

    bool RunWaveIn(const RecordSettings& settings, RecordResult& result) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            result.message = L"互換録音用の待機イベントを作成できませんでした。";
            return false;
        }
        HWAVEIN input = nullptr;
        MMRESULT mm = waveInOpen(&input, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(eventHandle), 0, CALLBACK_EVENT);
        if (mm != MMSYSERR_NOERROR) {
            format.nChannels = 1;
            format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            mm = waveInOpen(&input, WAVE_MAPPER, &format,
                reinterpret_cast<DWORD_PTR>(eventHandle), 0, CALLBACK_EVENT);
        }
        if (mm != MMSYSERR_NOERROR) {
            CloseHandle(eventHandle);
            result.message = L"WASAPIと互換録音の両方でデバイスを開けませんでした: " + WaveError(mm);
            return false;
        }
        event_ = eventHandle;

        std::error_code directoryError;
        fs::create_directories(settings.outputFolder, directoryError);
        if (directoryError) {
            result.message = L"保存先フォルダーを作成できませんでした。";
            waveInClose(input); event_ = nullptr; CloseHandle(eventHandle);
            return false;
        }
        result.path = UniqueOutputPath(settings);
        EncoderProcess encoder;
        if (!encoder.Start(settings, &format, result.path, result.message)) {
            waveInClose(input); event_ = nullptr; CloseHandle(eventHandle);
            return false;
        }

        constexpr size_t bufferCount = 8;
        constexpr size_t framesPerBuffer = 2048;
        std::vector<std::vector<char>> data(bufferCount,
            std::vector<char>(framesPerBuffer * format.nBlockAlign));
        std::vector<WAVEHDR> headers(bufferCount);
        bool ok = true;
        uint64_t capturedBytes = 0;
        for (size_t i = 0; i < bufferCount; ++i) {
            headers[i].lpData = data[i].data();
            headers[i].dwBufferLength = static_cast<DWORD>(data[i].size());
            if (waveInPrepareHeader(input, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(input, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                ok = false;
                result.message = L"互換録音のバッファーを準備できませんでした。";
                break;
            }
        }
        if (ok) {
            mm = waveInStart(input);
            if (mm != MMSYSERR_NOERROR) {
                ok = false;
                result.message = L"互換録音を開始できませんでした: " + WaveError(mm);
            } else if (g_window) {
                PostMessageW(g_window, WM_RECORDER_READY, 0, 0);
            }
        }
        while (ok && !stopRequested_) {
            WaitForSingleObject(eventHandle, 250);
            for (auto& header : headers) {
                if ((header.dwFlags & WHDR_DONE) == 0) continue;
                if (header.dwBytesRecorded > 0 &&
                    !encoder.Write(reinterpret_cast<const BYTE*>(header.lpData), header.dwBytesRecorded, result.message)) {
                    ok = false;
                    break;
                }
                capturedBytes += header.dwBytesRecorded;
                header.dwBytesRecorded = 0;
                if (!stopRequested_ && waveInAddBuffer(input, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                    ok = false;
                    result.message = L"互換録音のバッファーを再利用できませんでした。";
                    break;
                }
            }
        }

        waveInStop(input);
        waveInReset(input);
        for (auto& header : headers) {
            if ((header.dwFlags & WHDR_DONE) && header.dwBytesRecorded > 0 && ok) {
                ok = encoder.Write(reinterpret_cast<const BYTE*>(header.lpData), header.dwBytesRecorded, result.message);
                if (ok) capturedBytes += header.dwBytesRecorded;
            }
            if (header.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(input, &header, sizeof(WAVEHDR));
        }
        waveInClose(input);
        event_ = nullptr;
        CloseHandle(eventHandle);
        result.durationSeconds = format.nAvgBytesPerSec > 0
            ? static_cast<double>(capturedBytes) / format.nAvgBytesPerSec : 0.0;
        if (ok && encoder.Finish(result.message)) {
            result.ok = true;
            result.message = L"録音を保存しました（互換モード）。";
            return true;
        }
        return false;
    }

    void Complete(RecordResult* result, UINT message) {
        running_ = false;
        HWND window = g_window;
        if (window && PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(result))) return;
        delete result;
    }

    void Run(const RecordSettings& settings) {
        auto result = new RecordResult{};
        result->autoAdd = settings.autoAdd;
        result->layer = settings.targetLayer;
        result->frame = settings.targetFrame;

        // Windows requires the first IAudioClient activation to occur on an STA thread.
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool uninit = SUCCEEDED(init);
        auto finishCom = [&] { if (uninit) CoUninitialize(); };

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            result->message = L"録音デバイス機能を初期化できませんでした: " + WinError(hr);
            finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }
        ComPtr<IMMDevice> device;
        if (settings.deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        } else {
            hr = enumerator->GetDevice(settings.deviceId.c_str(), &device);
        }
        if (FAILED(hr)) {
            result->message = L"選択した録音デバイスを開けませんでした: " + WinError(hr);
            finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }

        ComPtr<IAudioClient> audio;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio);
        if (FAILED(hr)) {
            result->message = L"録音デバイスを開始できませんでした: " + WinError(hr);
            finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }
        WAVEFORMATEX* mix = nullptr;
        hr = audio->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            result->message = L"録音デバイスの音声形式を取得できませんでした: " + WinError(hr);
            finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }

        constexpr REFERENCE_TIME requestedBufferDuration = 10'000'000; // 1 second
        hr = audio->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            requestedBufferDuration, 0, mix, nullptr);
        if (FAILED(hr)) {
            WORD validBits = 0;
            DWORD channelMask = 0;
            GUID subFormat{};
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
                const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
                validBits = ext->Samples.wValidBitsPerSample;
                channelMask = ext->dwChannelMask;
                subFormat = ext->SubFormat;
            }
            const std::wstring wasapiError = std::format(
                L"録音デバイスを共有モードで開けませんでした: {} [tag={}, {}ch, {}Hz, {}bit, block={}, cb={}, valid={}, mask=0x{:X}, sub={:08X}]",
                WinError(hr), mix->wFormatTag, mix->nChannels, mix->nSamplesPerSec,
                mix->wBitsPerSample, mix->nBlockAlign, mix->cbSize, validBits, channelMask, subFormat.Data1);
            CoTaskMemFree(mix);
            LogInfo(wasapiError + L"; 互換録音へ切り替えます。");
            RunWaveIn(settings, *result);
            finishCom(); Complete(result, result->ok ? WM_RECORDER_DONE : WM_RECORDER_ERROR); return;
        }
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            result->message = L"録音待機イベントを作成できませんでした。";
            CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }
        event_ = eventHandle;
        hr = audio->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            result->message = L"録音待機イベントを登録できませんでした: " + WinError(hr);
            event_ = nullptr; CloseHandle(eventHandle); CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }
        ComPtr<IAudioCaptureClient> capture;
        hr = audio->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) {
            result->message = L"録音ストリームを取得できませんでした: " + WinError(hr);
            event_ = nullptr; CloseHandle(eventHandle); CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }

        std::error_code directoryError;
        fs::create_directories(settings.outputFolder, directoryError);
        if (directoryError) {
            result->message = L"保存先フォルダーを作成できませんでした。";
            event_ = nullptr; CloseHandle(eventHandle); CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }
        result->path = UniqueOutputPath(settings);
        EncoderProcess encoder;
        if (!encoder.Start(settings, mix, result->path, result->message)) {
            event_ = nullptr; CloseHandle(eventHandle); CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }

        hr = audio->Start();
        if (FAILED(hr)) {
            result->message = L"録音を開始できませんでした: " + WinError(hr);
            event_ = nullptr; CloseHandle(eventHandle); CoTaskMemFree(mix); finishCom(); Complete(result, WM_RECORDER_ERROR); return;
        }

        if (g_window) PostMessageW(g_window, WM_RECORDER_READY, 0, 0);

        bool streamOk = true;
        std::vector<BYTE> silence;
        uint64_t capturedFrames = 0;
        while (!stopRequested_) {
            WaitForSingleObject(eventHandle, 250);
            UINT32 packetFrames = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) { streamOk = false; result->message = L"録音データを取得できませんでした: " + WinError(hr); break; }
                DWORD bytes = frames * mix->nBlockAlign;
                const BYTE* source = data;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(bytes, 0);
                    source = silence.data();
                }
                if (!encoder.Write(source, bytes, result->message)) streamOk = false;
                if (streamOk) capturedFrames += frames;
                capture->ReleaseBuffer(frames);
                if (!streamOk) break;
            }
            if (!streamOk) break;
        }

        audio->Stop();
        result->durationSeconds = mix->nSamplesPerSec > 0
            ? static_cast<double>(capturedFrames) / mix->nSamplesPerSec : 0.0;
        event_ = nullptr;
        CloseHandle(eventHandle);
        CoTaskMemFree(mix);
        if (streamOk && encoder.Finish(result->message)) {
            result->ok = true;
            result->message = L"録音を保存しました。";
        } else if (result->message.empty()) {
            result->message = L"録音ファイルを完成できませんでした。";
        }
        finishCom();
        Complete(result, result->ok ? WM_RECORDER_DONE : WM_RECORDER_ERROR);
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<HANDLE> event_{nullptr};
    std::thread worker_;
};

Recorder g_recorder;

void AddComboItem(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void FillQualityCombo(int format, int selected = -1) {
    HWND combo = GetDlgItem(g_window, IDC_QUALITY);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    switch (format) {
    case 0: AddComboItem(combo, L"16-bit PCM"); AddComboItem(combo, L"24-bit PCM"); AddComboItem(combo, L"32-bit float"); break;
    case 1: AddComboItem(combo, L"標準 (圧縮レベル5)"); AddComboItem(combo, L"高圧縮 (レベル8)"); AddComboItem(combo, L"最高圧縮 (レベル12)"); break;
    case 2:
    case 3: AddComboItem(combo, L"128 kbps"); AddComboItem(combo, L"192 kbps"); AddComboItem(combo, L"256 kbps"); AddComboItem(combo, L"320 kbps"); break;
    case 4: AddComboItem(combo, L"64 kbps"); AddComboItem(combo, L"96 kbps"); AddComboItem(combo, L"128 kbps"); AddComboItem(combo, L"192 kbps"); AddComboItem(combo, L"256 kbps"); break;
    }
    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (selected < 0 || selected >= count) selected = (format == 4) ? 2 : 1;
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void RefreshDevices() {
    HWND combo = GetDlgItem(g_window, IDC_DEVICE);
    std::wstring previous;
    int old = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (old >= 0 && old < static_cast<int>(g_devices.size())) previous = g_devices[old].id;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_devices.clear();
    std::wstring defaultId;

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(init);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        ComPtr<IMMDevice> defaultDevice;
        LPWSTR rawDefaultId = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice)) &&
            SUCCEEDED(defaultDevice->GetId(&rawDefaultId))) {
            defaultId = rawDefaultId;
            CoTaskMemFree(rawDefaultId);
        }
        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr)) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device;
                LPWSTR id = nullptr;
                ComPtr<IPropertyStore> props;
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(collection->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
                    SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
                    SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
                    g_devices.push_back({id, name.pwszVal});
                    AddComboItem(combo, name.pwszVal);
                }
                if (id) CoTaskMemFree(id);
                PropVariantClear(&name);
            }
        }
    }
    if (uninit) CoUninitialize();
    int selection = 0;
    const std::wstring& preferred = previous.empty() ? defaultId : previous;
    for (size_t i = 0; i < g_devices.size(); ++i) if (g_devices[i].id == preferred) selection = static_cast<int>(i);
    if (!g_devices.empty()) SendMessageW(combo, CB_SETCURSEL, selection, 0);
    SetStatus(g_devices.empty() ? L"有効な録音デバイスがありません。" : L"待機中");
}

std::wstring ReadIniString(const wchar_t* key, const std::wstring& fallback) {
    if (g_iniPath.empty()) return fallback;
    std::wstring buffer(32768, L'\0');
    DWORD size = GetPrivateProfileStringW(L"EasyRec2", key, fallback.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), g_iniPath.c_str());
    buffer.resize(size);
    return buffer;
}

int ReadIniInt(const wchar_t* key, int fallback) {
    if (g_iniPath.empty()) return fallback;
    return GetPrivateProfileIntW(L"EasyRec2", key, fallback, g_iniPath.c_str());
}

void WriteIniInt(const wchar_t* key, int value) {
    if (g_iniPath.empty()) return;
    WritePrivateProfileStringW(L"EasyRec2", key, std::to_wstring(value).c_str(), g_iniPath.c_str());
}

void LoadSettings() {
    int format = ReadIniInt(L"format", 0);
    if (format < 0 || format > 4) format = 0;
    SendMessageW(GetDlgItem(g_window, IDC_FORMAT), CB_SETCURSEL, format, 0);
    FillQualityCombo(format, ReadIniInt(L"quality", 1));
    SendMessageW(GetDlgItem(g_window, IDC_RATE), CB_SETCURSEL, ReadIniInt(L"rateIndex", 1), 0);
    SendMessageW(GetDlgItem(g_window, IDC_CHANNELS), CB_SETCURSEL, ReadIniInt(L"channelsIndex", 1), 0);
    SetWindowTextW(GetDlgItem(g_window, IDC_FOLDER), ReadIniString(L"folder", L"").c_str());
    std::wstring ffmpeg = ReadIniString(L"ffmpeg", L"");
    if (ffmpeg.empty()) ffmpeg = FindFfmpeg();
    SetWindowTextW(GetDlgItem(g_window, IDC_FFMPEG), ffmpeg.c_str());
    Button_SetCheck(GetDlgItem(g_window, IDC_AUTO_ADD), ReadIniInt(L"autoAdd", 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(g_window, IDC_PLAY_PREVIEW), ReadIniInt(L"playPreview", 1) ? BST_CHECKED : BST_UNCHECKED);
    std::wstring wanted = ReadIniString(L"device", L"");
    for (size_t i = 0; i < g_devices.size(); ++i) {
        if (g_devices[i].id == wanted) SendMessageW(GetDlgItem(g_window, IDC_DEVICE), CB_SETCURSEL, i, 0);
    }
}

void SaveSettings() {
    if (g_iniPath.empty() || !g_window) return;
    int device = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_DEVICE), CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"EasyRec2", L"device", (device >= 0 && device < static_cast<int>(g_devices.size())) ? g_devices[device].id.c_str() : L"", g_iniPath.c_str());
    WriteIniInt(L"format", static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_FORMAT), CB_GETCURSEL, 0, 0)));
    WriteIniInt(L"quality", static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_QUALITY), CB_GETCURSEL, 0, 0)));
    WriteIniInt(L"rateIndex", static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_RATE), CB_GETCURSEL, 0, 0)));
    WriteIniInt(L"channelsIndex", static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_CHANNELS), CB_GETCURSEL, 0, 0)));
    WritePrivateProfileStringW(L"EasyRec2", L"folder", GetWindowTextString(GetDlgItem(g_window, IDC_FOLDER)).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"EasyRec2", L"ffmpeg", GetWindowTextString(GetDlgItem(g_window, IDC_FFMPEG)).c_str(), g_iniPath.c_str());
    WriteIniInt(L"autoAdd", Button_GetCheck(GetDlgItem(g_window, IDC_AUTO_ADD)) == BST_CHECKED ? 1 : 0);
    WriteIniInt(L"playPreview", Button_GetCheck(GetDlgItem(g_window, IDC_PLAY_PREVIEW)) == BST_CHECKED ? 1 : 0);
}

std::wstring DefaultRecordFolder() {
    PWSTR documents = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        result = (fs::path(documents) / L"AviUtl2 Recordings").wstring();
        CoTaskMemFree(documents);
    }
    return result;
}

bool PickFolder(HWND owner, std::wstring& selected) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"録音の保存先を選択");
    if (FAILED(dialog->Show(owner))) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    selected = path;
    CoTaskMemFree(path);
    return true;
}

bool PickFfmpeg(HWND owner, std::wstring& selected) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return false;
    COMDLG_FILTERSPEC filters[] = {{L"FFmpeg", L"ffmpeg.exe"}, {L"実行ファイル", L"*.exe"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"ffmpeg.exeを選択");
    if (FAILED(dialog->Show(owner))) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    selected = path;
    CoTaskMemFree(path);
    return true;
}

void SetRecordingUi(bool recording) {
    EnableWindow(GetDlgItem(g_window, IDC_START), !recording);
    EnableWindow(GetDlgItem(g_window, IDC_STOP), recording);
    for (int id : {IDC_DEVICE, IDC_REFRESH, IDC_FORMAT, IDC_RATE, IDC_CHANNELS, IDC_QUALITY,
                   IDC_FOLDER, IDC_BROWSE_FOLDER, IDC_FFMPEG, IDC_BROWSE_FFMPEG,
                   IDC_AUTO_ADD, IDC_PLAY_PREVIEW}) {
        EnableWindow(GetDlgItem(g_window, id), !recording);
    }
}

bool PostPreviewShortcut() {
    if (!g_hostWindow) return false;
    const UINT scan = MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC);
    SetFocus(g_hostWindow);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    const LPARAM up = down | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    return PostMessageW(g_hostWindow, WM_KEYDOWN, VK_SPACE, down) &&
           PostMessageW(g_hostWindow, WM_KEYUP, VK_SPACE, up);
}

void StartPreviewAtRecordedPosition() {
    if (!g_edit || !g_window || !g_recorder.IsRunning()) return;
    if (g_edit->get_edit_state() == EDIT_HANDLE::EDIT_STATE_PLAY) {
        g_previewStartPending = false;
        g_previewTogglePosted = false;
        SetStatus(L"● 録音中（既に再生中）");
        return;
    }
    struct CursorRequest { int layer; int frame; } request{g_recordStartLayer, g_recordStartFrame};
    g_edit->call_edit_section_param(&request, [](void* raw, EDIT_SECTION* edit) {
        auto* position = static_cast<CursorRequest*>(raw);
        edit->set_cursor_layer_frame(position->layer, position->frame);
    });
    if (PostPreviewShortcut()) {
        g_previewStartPending = true;
        g_previewTogglePosted = true;
        g_previewCheckAttempts = 0;
        SetTimer(g_window, TIMER_PLAYBACK_CHECK, 150, nullptr);
    } else {
        g_previewStartPending = false;
        g_previewTogglePosted = false;
        SetStatus(L"● 録音中（プレビューはSpaceキーで再生してください）");
    }
}

void StopPreviewIfOwned() {
    if (g_window) KillTimer(g_window, TIMER_PLAYBACK_CHECK);
    const bool mayOwnPlayback = g_previewStartedByPlugin || g_previewTogglePosted;
    g_previewStartPending = false;
    g_previewTogglePosted = false;
    if (mayOwnPlayback && g_edit &&
        g_edit->get_edit_state() == EDIT_HANDLE::EDIT_STATE_PLAY) {
        PostPreviewShortcut();
    }
    g_previewStartedByPlugin = false;
}

RecordSettings CollectSettings() {
    RecordSettings value;
    int device = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_DEVICE), CB_GETCURSEL, 0, 0));
    if (device >= 0 && device < static_cast<int>(g_devices.size())) value.deviceId = g_devices[device].id;
    value.outputFolder = GetWindowTextString(GetDlgItem(g_window, IDC_FOLDER));
    value.ffmpegPath = GetWindowTextString(GetDlgItem(g_window, IDC_FFMPEG));
    value.formatIndex = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_FORMAT), CB_GETCURSEL, 0, 0));
    int rate = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_RATE), CB_GETCURSEL, 0, 0));
    static constexpr int rates[] = {44100, 48000, 96000, 192000};
    value.rate = rates[(rate >= 0 && rate < 4) ? rate : 1];
    int channels = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_CHANNELS), CB_GETCURSEL, 0, 0));
    value.channels = channels == 0 ? 1 : 2;
    value.qualityIndex = static_cast<int>(SendMessageW(GetDlgItem(g_window, IDC_QUALITY), CB_GETCURSEL, 0, 0));
    value.autoAdd = Button_GetCheck(GetDlgItem(g_window, IDC_AUTO_ADD)) == BST_CHECKED;
    value.playPreview = Button_GetCheck(GetDlgItem(g_window, IDC_PLAY_PREVIEW)) == BST_CHECKED;
    if (g_edit) {
        EDIT_INFO info{};
        g_edit->get_edit_info(&info, sizeof(info));
        value.targetLayer = info.layer;
        value.targetFrame = info.frame;
    }
    return value;
}

void StartRecording() {
    if (g_devices.empty()) { MessageBoxW(g_window, L"有効な録音デバイスがありません。", kWindowTitle, MB_ICONWARNING); return; }
    RecordSettings settings = CollectSettings();
    if (settings.outputFolder.empty()) {
        settings.outputFolder = DefaultRecordFolder();
        SetWindowTextW(GetDlgItem(g_window, IDC_FOLDER), settings.outputFolder.c_str());
    }
    if (settings.ffmpegPath.empty() || !fs::exists(settings.ffmpegPath)) {
        MessageBoxW(g_window, L"FFmpegが見つかりません。ffmpeg.exeの場所を指定してください。", kWindowTitle, MB_ICONWARNING);
        return;
    }
    SaveSettings();
    g_recordStartLayer = settings.targetLayer;
    g_recordStartFrame = settings.targetFrame;
    const bool playPreview = settings.playPreview;
    if (g_recorder.Start(std::move(settings))) {
        StopRecordingClock();
        SetRecordingTime(0.0);
        g_previewStartedByPlugin = false;
        g_previewTogglePosted = false;
        g_previewStartPending = playPreview;
        SetRecordingUi(true);
        SetStatus(L"録音デバイスを準備しています…");
    }
}

void AddRecordingToTimeline(RecordResult& result) {
    if (!g_edit || !result.autoAdd) return;
    struct AddRequest {
        const wchar_t* path;
        int layer;
        int frame;
        double capturedDuration;
        bool added;
        int actualLayer;
        int length;
    } request{result.path.c_str(), result.layer, result.frame, result.durationSeconds, false, result.layer, 1};
    bool called = g_edit->call_edit_section_param(&request, [](void* raw, EDIT_SECTION* edit) {
        auto* r = static_cast<AddRequest*>(raw);
        if (!edit->is_support_media_file(r->path, false)) return;
        double duration = r->capturedDuration;
        if (duration <= 0.0) {
            MEDIA_INFO media{};
            if (edit->get_media_info(r->path, &media, sizeof(media)) && media.total_time > 0.0) {
                duration = media.total_time;
            }
        }
        if (edit->info && edit->info->rate > 0 && edit->info->scale > 0 && duration > 0.0) {
            r->length = std::max(1, static_cast<int>(std::ceil(duration * edit->info->rate / edit->info->scale)));
        }
        for (int layer = r->layer; layer < r->layer + 100; ++layer) {
            if (edit->create_object_from_media_file(r->path, layer, r->frame, r->length)) {
                r->added = true;
                r->actualLayer = layer;
                return;
            }
        }
    });
    if (!called || !request.added) {
        result.message += L" タイムラインへの自動追加には対応する入力プラグインと空きレイヤーが必要です。";
    } else if (request.actualLayer != request.layer) {
        result.message += std::format(L" 空きのあるレイヤー{}へ追加しました。", request.actualLayer + 1);
    } else {
        result.message += L" タイムラインへ追加しました。";
    }
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
        g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_module, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

void CreateControls() {
    MakeControl(WC_STATICW, L"録音デバイス", 0, 12, 14, 105, 22, 0);
    MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 120, 10, 335, 220, IDC_DEVICE);
    MakeControl(WC_BUTTONW, L"更新", BS_PUSHBUTTON, 462, 10, 58, 25, IDC_REFRESH);

    MakeControl(WC_STATICW, L"保存形式", 0, 12, 51, 105, 22, 0);
    HWND format = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 120, 47, 150, 180, IDC_FORMAT);
    for (const wchar_t* item : {L"WAV", L"FLAC", L"MP3", L"AAC / M4A", L"Opus"}) AddComboItem(format, item);
    MakeControl(WC_STATICW, L"音質", 0, 282, 51, 45, 22, 0);
    MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 330, 47, 190, 180, IDC_QUALITY);

    MakeControl(WC_STATICW, L"サンプルレート", 0, 12, 88, 105, 22, 0);
    HWND rate = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 120, 84, 150, 150, IDC_RATE);
    for (const wchar_t* item : {L"44.1 kHz", L"48 kHz", L"96 kHz", L"192 kHz"}) AddComboItem(rate, item);
    MakeControl(WC_STATICW, L"チャンネル", 0, 282, 88, 70, 22, 0);
    HWND channels = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 355, 84, 165, 100, IDC_CHANNELS);
    AddComboItem(channels, L"モノラル"); AddComboItem(channels, L"ステレオ");

    MakeControl(WC_STATICW, L"保存先", 0, 12, 125, 105, 22, 0);
    MakeControl(WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL, 120, 121, 335, 25, IDC_FOLDER);
    MakeControl(WC_BUTTONW, L"参照", BS_PUSHBUTTON, 462, 121, 58, 25, IDC_BROWSE_FOLDER);

    MakeControl(WC_STATICW, L"FFmpeg", 0, 12, 162, 105, 22, 0);
    MakeControl(WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL, 120, 158, 335, 25, IDC_FFMPEG);
    MakeControl(WC_BUTTONW, L"参照", BS_PUSHBUTTON, 462, 158, 58, 25, IDC_BROWSE_FFMPEG);

    MakeControl(WC_BUTTONW, L"録音開始と同時に現在位置からプレビュー再生", BS_AUTOCHECKBOX, 120, 193, 400, 24, IDC_PLAY_PREVIEW);
    MakeControl(WC_BUTTONW, L"録音後にタイムラインへ自動追加（現在位置／空きレイヤー）", BS_AUTOCHECKBOX, 120, 219, 400, 24, IDC_AUTO_ADD);
    MakeControl(WC_STATICW, L"録音時間 00:00.0", SS_CENTER, 12, 254, 168, 28, IDC_DURATION);
    MakeControl(WC_STATICW, L"待機中", SS_LEFT, 192, 254, 328, 42, IDC_STATUS);
    MakeControl(WC_BUTTONW, L"録音開始", BS_DEFPUSHBUTTON, 12, 306, 150, 34, IDC_START);
    MakeControl(WC_BUTTONW, L"停止", BS_PUSHBUTTON, 174, 306, 100, 34, IDC_STOP);
    MakeControl(WC_BUTTONW, L"保存先を開く", BS_PUSHBUTTON, 370, 306, 150, 34, IDC_OPEN_FOLDER);
    EnableWindow(GetDlgItem(g_window, IDC_STOP), FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        int id = LOWORD(wparam);
        int notification = HIWORD(wparam);
        if (id == IDC_REFRESH && notification == BN_CLICKED) RefreshDevices();
        else if (id == IDC_FORMAT && notification == CBN_SELCHANGE) {
            FillQualityCombo(static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_FORMAT), CB_GETCURSEL, 0, 0)));
        } else if (id == IDC_BROWSE_FOLDER && notification == BN_CLICKED) {
            std::wstring path;
            if (PickFolder(hwnd, path)) SetWindowTextW(GetDlgItem(hwnd, IDC_FOLDER), path.c_str());
        } else if (id == IDC_BROWSE_FFMPEG && notification == BN_CLICKED) {
            std::wstring path;
            if (PickFfmpeg(hwnd, path)) SetWindowTextW(GetDlgItem(hwnd, IDC_FFMPEG), path.c_str());
        } else if (id == IDC_START && notification == BN_CLICKED) StartRecording();
        else if (id == IDC_STOP && notification == BN_CLICKED) {
            StopRecordingClock();
            SetStatus(L"録音を終了しています…");
            EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);
            g_recorder.RequestStop();
            StopPreviewIfOwned();
        } else if (id == IDC_OPEN_FOLDER && notification == BN_CLICKED) {
            std::wstring folder = GetWindowTextString(GetDlgItem(hwnd, IDC_FOLDER));
            if (!folder.empty()) ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_RECORDER_READY:
        if (!IsWindowEnabled(GetDlgItem(hwnd, IDC_STOP))) return 0;
        StartRecordingClock();
        if (g_previewStartPending) StartPreviewAtRecordedPosition();
        else SetStatus(L"● 録音中…");
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_RECORDING_CLOCK && g_recordingClockStarted) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_recordingStartTime).count();
            SetRecordingTime(elapsed);
            return 0;
        }
        if (wparam == TIMER_PLAYBACK_CHECK && g_previewStartPending && g_edit) {
            if (g_edit->get_edit_state() == EDIT_HANDLE::EDIT_STATE_PLAY) {
                g_previewStartedByPlugin = true;
                g_previewStartPending = false;
                g_previewTogglePosted = false;
                KillTimer(hwnd, TIMER_PLAYBACK_CHECK);
                SetStatus(L"● 録音中（プレビュー再生中）");
            } else if (++g_previewCheckAttempts >= 5) {
                g_previewStartPending = false;
                g_previewTogglePosted = false;
                KillTimer(hwnd, TIMER_PLAYBACK_CHECK);
                SetStatus(L"● 録音中（再生できない場合はSpaceキーを押してください）");
            }
        }
        return 0;
    case WM_RECORDER_DONE:
    case WM_RECORDER_ERROR: {
        std::unique_ptr<RecordResult> result(reinterpret_cast<RecordResult*>(lparam));
        StopRecordingClock();
        if (result && result->ok && result->durationSeconds > 0.0) SetRecordingTime(result->durationSeconds);
        StopPreviewIfOwned();
        if (result && result->ok) AddRecordingToTimeline(*result);
        SetRecordingUi(false);
        SetStatus(result ? result->message : L"録音処理が終了しました。");
        if (result && !result->ok) {
            LogError(result->message);
        } else if (result) {
            LogInfo(result->path);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_PLAYBACK_CHECK);
        KillTimer(hwnd, TIMER_RECORDING_CLOCK);
        g_recordingClockStarted = false;
        SaveSettings();
        return 0;
    case WM_NCDESTROY:
        g_window = nullptr;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

extern "C" __declspec(dllexport) DWORD RequiredVersion() {
    return 2010000;
}

extern "C" __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    g_logger = logger;
}

extern "C" __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) {
    g_config = config;
    if (config && config->app_data_path) {
        fs::path directory = fs::path(config->app_data_path) / L"EasyRec2";
        std::error_code ec;
        fs::create_directories(directory, ec);
        g_iniPath = (directory / L"settings.ini").wstring();
    }
}

extern "C" __declspec(dllexport) bool InitializePlugin(DWORD) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(hr);
    return true;
}

extern "C" __declspec(dllexport) void UninitializePlugin() {
    g_recorder.Stop();
    StopRecordingClock();
    if (g_window) SaveSettings();
    g_window = nullptr;
    g_edit = nullptr;
    g_hostWindow = nullptr;
    if (g_comInitialized) {
        CoUninitialize();
        g_comInitialized = false;
    }
}

extern "C" __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    static COMMON_PLUGIN_TABLE table = {
        L"簡易録音2",
        L"簡易録音2 v0.1.2 - WASAPI / FFmpeg audio recorder for AviUtl ExEdit2"
    };
    return &table;
}

extern "C" __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_module;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    g_window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 544, 382, nullptr, nullptr, g_module, nullptr);
    if (!g_window) return;
    CreateControls();
    RefreshDevices();
    if (GetWindowTextLengthW(GetDlgItem(g_window, IDC_FOLDER)) == 0) {
        SetWindowTextW(GetDlgItem(g_window, IDC_FOLDER), DefaultRecordFolder().c_str());
    }
    LoadSettings();
    if (GetWindowTextLengthW(GetDlgItem(g_window, IDC_FOLDER)) == 0) {
        SetWindowTextW(GetDlgItem(g_window, IDC_FOLDER), DefaultRecordFolder().c_str());
    }
    host->register_window_client(kWindowTitle, g_window);
    g_edit = host->create_edit_handle();
    if (g_edit) g_hostWindow = g_edit->get_host_app_window();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
