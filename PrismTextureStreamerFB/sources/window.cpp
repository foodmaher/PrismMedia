#define _CRT_SECURE_NO_WARNINGS // Thanks microsoft

#include "window.h"

#include "../diagnostic_log.h"
#include "../scs_logging.h"
#include "../thread_scheduling.h"
using namespace scs_logging;

#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <thread>

struct FindWindowData {
    const char* exeName;
    const char* windowTitle;
    HWND result;
};
static HWND FindWindowByNameAndTitle(const char* exeName, const char* windowTitle)
{
    FindWindowData data{ exeName, windowTitle, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* data = reinterpret_cast<FindWindowData*>(lParam);

        if (!IsWindowVisible(hwnd))
            return TRUE;

        LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW)
            return TRUE;

        bool titleMatch{};
        if (data->windowTitle) {
            LRESULT lengthResult = 0;
            if (!SendMessageTimeoutA(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, (PDWORD_PTR)&lengthResult)) {
                return TRUE; // didn't respond in time - skip it
            }
            int titleLen = static_cast<int>(lengthResult);

            if (titleLen != 0)
            {
                std::string windowTitle = std::string(titleLen + 1, '\0');
                GetWindowTextA(hwnd, windowTitle.data(), titleLen + 1);
                windowTitle.resize(titleLen);

                if (windowTitle == std::string_view(data->windowTitle))
                    titleMatch = true; // Title matches, but so must the exe name
            }
            else
                return TRUE; // Window title was given as a search filter, so if no title, skip
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return TRUE;

        char path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameA(hProc, 0, path, &size);
        CloseHandle(hProc);

        std::string_view applicationName(path);
        auto pos = applicationName.rfind('\\');
        applicationName = pos != std::string::npos ? applicationName.substr(pos + 1) : applicationName;

        bool exeMatch = applicationName == std::string_view(data->exeName);

        if (exeMatch && (!data->windowTitle || titleMatch)) {
            data->result = hwnd;
            return FALSE; // Exe matches, and there is either no title, or the title matched
        }

        return TRUE;
        }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}


namespace sources {
    class WindowSource : public IContentSource
    {
    private:
        char* m_appname{};
        char* m_apptitle{};
        HWND m_hwnd{};
        std::atomic<uint32_t> m_width{};
        std::atomic<uint32_t> m_height{};
        std::atomic<uint8_t> m_framerate{ 30 };
        std::atomic<bool> m_paused{};
        std::atomic<uint32_t> m_outputWidth{ 1280 };
        std::atomic<uint32_t> m_outputHeight{ 720 };

        std::vector<uint8_t> m_frameBuffer;
        std::vector<uint32_t> m_scaleX;
        uint32_t m_scaleSourceWidth{};
        uint32_t m_scaleOutputWidth{};
        std::mutex m_bufferMutex;

        std::atomic<bool> m_haveFrame{};
        uint64_t m_frameGeneration{};
        uint64_t m_lastCopiedGeneration{};
        std::chrono::steady_clock::time_point m_lastDelivered{};
        std::atomic<double> m_workerCpuMs{};
        std::atomic<double> m_deliveredFps{};
        std::atomic<uint64_t> m_droppedFrames{};
        uint64_t m_lastErrorLogTick{};
        uint32_t m_lastLoggedWidth{};
        uint32_t m_lastLoggedHeight{};
        bool m_wasMinimized{};

        std::thread m_thread;
        std::atomic<bool> m_stopRequested{};

        void CaptureLoop()
        {
            thread_scheduling::refresh_current_thread_preference();
            RECT initialRect{};
            GetClientRect(m_hwnd, &initialRect);
            const uint32_t initialWidth = (std::max)(
                1L, initialRect.right - initialRect.left);
            const uint32_t initialHeight = (std::max)(
                1L, initialRect.bottom - initialRect.top);

            HDC windowDC = GetDC(m_hwnd);
            HDC memDC = CreateCompatibleDC(windowDC);
            HBITMAP bitmap = CreateCompatibleBitmap(windowDC, initialWidth, initialHeight);
            HGDIOBJ oldObj = SelectObject(memDC, bitmap);

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(initialWidth);
            bmi.bmiHeader.biHeight = -static_cast<LONG>(initialHeight);
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;


            const size_t arraysize = static_cast<size_t>(initialWidth) * initialHeight * 4;
            std::vector<uint8_t> bgraScratch(arraysize);
            m_frameBuffer.resize(arraysize);

            while (!m_stopRequested.load())
            {
                thread_scheduling::refresh_current_thread_preference();
                const auto fps = (std::max)(static_cast<uint8_t>(1), m_framerate.load());
                const auto frameInterval = std::chrono::milliseconds(1000 / fps);
                auto frameStart = std::chrono::steady_clock::now();

                if (!IsWindow(m_hwnd)) {
                    scs_log(0, "[WindowSource] Target window %s (%s) no longer exists, stopping capture for this window", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");
                    diagnostic_log::writef(
                        "error", "Legacy capture target disappeared: %s (%s).",
                        m_appname, m_apptitle ? m_apptitle : "no title");
                    break;
                }
                if (m_paused.load() && m_haveFrame.load()) { std::this_thread::sleep_for(frameInterval); continue; }
                if (IsIconic(m_hwnd)) {
                    if (!m_wasMinimized)
                    {
                        diagnostic_log::writef(
                            "capture", "Legacy capture target %s was minimized; "
                            "keeping the last frame.", m_appname);
                        m_wasMinimized = true;
                    }
                    std::this_thread::sleep_for(frameInterval);
                    continue;
                }
                if (m_wasMinimized)
                {
                    diagnostic_log::writef(
                        "capture", "Legacy capture target %s was restored.",
                        m_appname);
                    m_wasMinimized = false;
                }

                RECT rect;
                GetClientRect(m_hwnd, &rect);
                const uint32_t width = rect.right - rect.left;
                const uint32_t height = rect.bottom - rect.top;

                if (width != m_lastLoggedWidth || height != m_lastLoggedHeight)
                {
                    diagnostic_log::writef(
                        "capture", "Legacy capture %s size is now %ux%u.",
                        m_appname, width, height);
                    m_lastLoggedWidth = width;
                    m_lastLoggedHeight = height;
                }

                bmi.bmiHeader.biWidth = static_cast<LONG>(width);
                bmi.bmiHeader.biHeight = -static_cast<LONG>(height);

                const size_t arraysize = static_cast<size_t>(width) * height * 4;
                if (bgraScratch.size() != arraysize)
                {
                    bgraScratch.resize(arraysize);
                    SelectObject(memDC, oldObj); // deselect current bitmap before deleting
                    DeleteObject(bitmap);
                    bitmap = CreateCompatibleBitmap(windowDC, width, height);
                    SelectObject(memDC, bitmap);
                }

                BOOL pwOk = PrintWindow(m_hwnd, memDC, 2 /* PW_RENDERFULLCONTENT */);
                if (!pwOk)
                {
                    scs_log(0, "[WindowSource] PrintWindow failed for %s (%s), err=%lu", m_appname, m_apptitle ? m_apptitle : "NO_TITLE", GetLastError());
                    const uint64_t tick = GetTickCount64();
                    if (m_lastErrorLogTick == 0 ||
                        tick - m_lastErrorLogTick >= 5000)
                    {
                        diagnostic_log::writef(
                            "error", "PrintWindow failed for %s (Win32 %lu).",
                            m_appname, GetLastError());
                        m_lastErrorLogTick = tick;
                    }
                    std::this_thread::sleep_for(frameInterval);
                    continue;
                }
                GetDIBits(memDC, bitmap, 0, height, bgraScratch.data(), &bmi, DIB_RGB_COLORS);

                uint32_t outputWidth = width;
                uint32_t outputHeight = height;
                const uint32_t maxOutputWidth = (std::max)(1U, m_outputWidth.load());
                const uint32_t maxOutputHeight = (std::max)(1U, m_outputHeight.load());
                if (outputWidth > maxOutputWidth || outputHeight > maxOutputHeight)
                {
                    if (static_cast<uint64_t>(width) * maxOutputHeight >
                        static_cast<uint64_t>(height) * maxOutputWidth)
                    {
                        outputWidth = maxOutputWidth;
                        outputHeight = (std::max)(1U, static_cast<uint32_t>(
                            static_cast<uint64_t>(height) * outputWidth / width));
                    }
                    else
                    {
                        outputHeight = maxOutputHeight;
                        outputWidth = (std::max)(1U, static_cast<uint32_t>(
                            static_cast<uint64_t>(width) * outputHeight / height));
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);

                    const size_t outputSize =
                        static_cast<size_t>(outputWidth) * outputHeight * 4;
                    if (m_frameBuffer.size() != outputSize)
                        m_frameBuffer.resize(outputSize);

                    const auto* srcPixels = reinterpret_cast<const uint32_t*>(bgraScratch.data());
                    auto* dstPixels = reinterpret_cast<uint32_t*>(m_frameBuffer.data());
                    if (m_scaleSourceWidth != width || m_scaleOutputWidth != outputWidth)
                    {
                        m_scaleX.resize(outputWidth);
                        for (uint32_t x = 0; x < outputWidth; ++x)
                        {
                            m_scaleX[x] = static_cast<uint32_t>(
                                static_cast<uint64_t>(x) * width / outputWidth);
                        }
                        m_scaleSourceWidth = width;
                        m_scaleOutputWidth = outputWidth;
                    }

                    for (uint32_t y = 0; y < outputHeight; ++y)
                    {
                        const uint32_t sourceY = static_cast<uint32_t>(
                            static_cast<uint64_t>(y) * height / outputHeight);
                        for (uint32_t x = 0; x < outputWidth; ++x)
                        {
                            dstPixels[static_cast<size_t>(y) * outputWidth + x] =
                                srcPixels[static_cast<size_t>(sourceY) * width + m_scaleX[x]] |
                                0xFF000000U;
                        }
                    }
                    m_width = outputWidth;
                    m_height = outputHeight;
                    ++m_frameGeneration;
                    m_haveFrame = true;
                }

                auto elapsed = std::chrono::steady_clock::now() - frameStart;
                const double workerMs =
                    std::chrono::duration<double, std::milli>(elapsed).count();
                const double oldWorker = m_workerCpuMs.load();
                m_workerCpuMs = oldWorker == 0.0
                    ? workerMs : oldWorker * 0.90 + workerMs * 0.10;

                const auto deliveredAt = std::chrono::steady_clock::now();
                if (m_lastDelivered.time_since_epoch().count() != 0)
                {
                    const double interval = std::chrono::duration<double>(
                        deliveredAt - m_lastDelivered).count();
                    if (interval > 0.0)
                    {
                        const double instantaneousFps = 1.0 / interval;
                        const double oldFps = m_deliveredFps.load();
                        m_deliveredFps = oldFps == 0.0
                            ? instantaneousFps : oldFps * 0.90 + instantaneousFps * 0.10;
                    }
                }
                m_lastDelivered = deliveredAt;
                if (elapsed < frameInterval) std::this_thread::sleep_for(frameInterval - elapsed);
            }

            SelectObject(memDC, oldObj);
            DeleteObject(bitmap);
            DeleteDC(memDC);
            ReleaseDC(m_hwnd, windowDC);

            scs_log(0, "[WindowSource] Source for %s (%s) has stopped", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");
            diagnostic_log::writef(
                "capture", "Legacy capture stopped: %s (%s).",
                m_appname, m_apptitle ? m_apptitle : "no title");
        }

    public:
        explicit WindowSource(
            const char* application_name,
            const char* application_title,
            uint32_t output_width,
            uint32_t output_height)
        {
            SetOutputSize(output_width, output_height);
            // Create our own ownership
            m_appname = new char[strlen(application_name) + 1]{};
            strcpy(m_appname, application_name);

            if (application_title) {
                m_apptitle = new char[strlen(application_title) + 1] {};
                strcpy(m_apptitle, application_title);
            }
        }
        ~WindowSource() override
        {
            m_stopRequested = true;
            if (m_thread.joinable()) m_thread.join();

            if (m_appname) delete[] m_appname;
            if (m_apptitle) delete[] m_apptitle;
        }


        bool Start(uint8_t framerate)
        {
            m_framerate = framerate;
            m_hwnd = FindWindowByNameAndTitle(m_appname, m_apptitle);
            if (!m_hwnd) {
                scs_log(2, "[WindowSource] Application %s (%s) not found at source startup", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");
                diagnostic_log::writef(
                    "error", "Legacy capture target not found: %s (%s).",
                    m_appname, m_apptitle ? m_apptitle : "no title");
                return false;
            }

            m_thread = std::thread(&WindowSource::CaptureLoop, this);

            scs_log(0, "[WindowSource] Source for %s (%s) has started", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");
            diagnostic_log::writef(
                "capture", "Legacy capture started: %s (%s), target %ux%u "
                "at %u FPS.",
                m_appname, m_apptitle ? m_apptitle : "no title",
                m_outputWidth.load(), m_outputHeight.load(),
                static_cast<unsigned>(m_framerate.load()));
            return true;
        }

        uint32_t GetWidth() const override { return m_width.load(); }
        uint32_t GetHeight() const override { return m_height.load(); }
        void SetFramerate(uint8_t framerate) override { m_framerate = (std::max)(static_cast<uint8_t>(1), framerate); }
        void SetPaused(bool paused) override { m_paused = paused; }
        void SetOutputSize(uint32_t width, uint32_t height) override
        {
            m_outputWidth = (std::max)(1U, width);
            m_outputHeight = (std::max)(1U, height);
        }

        bool CopyLatestFrame(std::vector<uint8_t>& dst, uint32_t& width, uint32_t& height) override
        {
            if (!m_haveFrame.load()) return false;

            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (m_lastCopiedGeneration == m_frameGeneration)
                return false;
            dst.swap(m_frameBuffer);
            width = m_width.load();
            height = m_height.load();
            m_lastCopiedGeneration = m_frameGeneration;
            return true;
        }

        source_performance_stats_t GetPerformanceStats() const override
        {
            source_performance_stats_t result;
            result.workerCpuMs = m_workerCpuMs.load();
            result.deliveredFps = m_deliveredFps.load();
            result.droppedFrames = m_droppedFrames.load();
            return result;
        }
    };


	std::unique_ptr<IContentSource> CreateWindowSource(
        const char* application_name,
        const char* application_title,
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height)
	{
		auto src = std::make_unique<WindowSource>(
            application_name, application_title, output_width, output_height);
		if (!src->Start(framerate)) return nullptr;
		return src;
	}
}
