#include "stdafx.h"

#pragma warning(push, 0)
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <wrl.h>

#include <libPPUI/win32_op.h>
#pragma warning(pop)

#include "logging.h"
#include "math_util.h"
#include "metadb_index_search_avoidance.h"
#include "metrics.h"
#include "preferences.h"
#include "ui_hooks.h"
#include "ui_lyrics_panel.h"

#include "timer_block.h"

static const GUID GUID_EXTERNAL_WINDOW_WAS_OPEN = { 0x8af70b0e, 0x4c7, 0x423b, { 0xa5, 0x59, 0x9e, 0x53, 0x65, 0xd8, 0x28, 0x29 } };
static const GUID GUID_EXTERNAL_WINDOW_PREVIOUS_X = { 0x9f9f3255, 0xefd2, 0x4cbd, { 0x91, 0xd0, 0xd5, 0xcb, 0xa0, 0x2c, 0xd1, 0xb5 } };
static const GUID GUID_EXTERNAL_WINDOW_PREVIOUS_Y = { 0x203d9d71, 0x7ee3, 0x4f36, { 0xb4, 0xe, 0x6e, 0x3e, 0x28, 0x76, 0xa0, 0x3a } };
static const GUID GUID_EXTERNAL_WINDOW_PREVIOUS_SIZE_X = { 0x66fabbce, 0x2594, 0x426a, { 0xa0, 0x9d, 0x2e, 0xa5, 0x53, 0x8d, 0x8f, 0x10 } };
static const GUID GUID_EXTERNAL_WINDOW_PREVIOUS_SIZE_Y = { 0x808ad0e1, 0xb60c, 0x4c77, { 0xb9, 0xb7, 0x5e, 0xe3, 0xaa, 0xc7, 0x2a, 0xfb } };
static cfg_int_t<uint64_t> cfg_external_window_was_open(GUID_EXTERNAL_WINDOW_WAS_OPEN, 0);
static cfg_int_t<int> cfg_external_window_previous_x(GUID_EXTERNAL_WINDOW_PREVIOUS_X, 0);
static cfg_int_t<int> cfg_external_window_previous_y(GUID_EXTERNAL_WINDOW_PREVIOUS_Y, 0);
static cfg_int_t<int> cfg_external_window_previous_size_x(GUID_EXTERNAL_WINDOW_PREVIOUS_SIZE_X, 0);
static cfg_int_t<int> cfg_external_window_previous_size_y(GUID_EXTERNAL_WINDOW_PREVIOUS_SIZE_Y, 0);

struct D2DTextRenderContext
{
    ID2D1DeviceContext* device;
    IDWriteFontFace1* fontface;
    ID2D1SolidColorBrush* brush;
    IDWriteTextFormat* text_format;

    float pixels_per_em;
    float pixels_per_design_unit;

    int font_ascent_px;
    int font_descent_px;
};

class ExternalLyricWindow : public LyricPanel
{
public:
    ExternalLyricWindow();
    ~ExternalLyricWindow() override;
    void SetUp();
    void SetUpDX(bool force);

    LRESULT OnWindowCreate(LPCREATESTRUCT) override;
    void OnWindowDestroy() override;
    void OnWindowMove(CPoint new_origin) override;
    void OnWindowResize(UINT request_type, CSize new_size) override;

    void OnPaint(CDCHandle) override;

    bool is_panel_ui_in_edit_mode() override;
    void compute_background_image() override;

private:
    void DrawNoLyrics(D2DTextRenderContext& render);
    void DrawUntimedLyrics(LyricData& lyrics, D2DTextRenderContext& render);
    void DrawTimestampedLyrics(D2DTextRenderContext& render);

    HMODULE m_direct_composition = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap_chain = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3d_device = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2d_device = nullptr;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2d_dc = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_d2d_bitmap = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcomp_device = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcomp_target = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcomp_visual = nullptr;

    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_d2d_albumart_bitmap = nullptr;
};

static ExternalLyricWindow* g_external_window = nullptr;


ExternalLyricWindow::ExternalLyricWindow()
    : LyricPanel()
{
    metrics::log_used_external_window();

    // NOTE: We manually load this because it's only available from Windows 8 onwards
    //       and fb2k supports Windows 7. It's only required for the transparent window
    //       background, and we'd rather not make all of openlyrics unavailable to the
    //       few remaining Win7 users just for that. So we load it dynamically at
    //       runtime and just don't support transparent backgrounds on Win7.
    //       If we later drop support for Win7, we can remove all of this and link
    //       to DirectComposition at compile time.
    m_direct_composition = LoadLibrary(_T("Dcomp.dll"));
}

ExternalLyricWindow::~ExternalLyricWindow()
{
    if(m_direct_composition != nullptr)
    {
        FreeLibrary(m_direct_composition);
        m_direct_composition = nullptr;
    }
}

void ExternalLyricWindow::SetUp()
{
    const HWND parent = nullptr;
    const TCHAR* window_name = _T("OpenLyrics external window");
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD ex_style = (m_direct_composition == nullptr) ? 0 : WS_EX_NOREDIRECTIONBITMAP;

    // NOTE: We specifically need to exclude the WS_VISIBLE style (which causes the window
    //       to be created already-visible) because including it results in ColumnsUI
    //       logging a warning that the "panel was visible on creation".
    //       It would seem that even without this style and without us making the panel
    //       visible after creation, fb2k does this for us already.
    //       See: https://github.com/jacquesh/foo_openlyrics/issues/132

    WIN32_OP(Create(parent, nullptr, window_name, style, ex_style) != NULL)

    // NOTE: We need to do this separately because the rect passed to CreateWindow appears
    //       to include the non-client area, whereas all our other measurements are
    //       specifically for the client area. So if we were to pass this size as-is to
    //       CreateWindow then it would create a window that is slightly smaller than we
    //       intended, and that size would be saved so the next window would again be
    //       slightly smaller etc until we have just a thin line for our window.
    BOOL success = SetWindowPos(
            HWND_TOPMOST,
            cfg_external_window_previous_x.get_value(),
            cfg_external_window_previous_y.get_value(),
            cfg_external_window_previous_size_x.get_value(),
            cfg_external_window_previous_size_y.get_value(),
            SWP_NOMOVE | SWP_NOSIZE);
    if(!success)
    {
        const auto GetLastErrorString = []() -> const char*
        {
            DWORD error = GetLastError();
            static char errorMsgBuffer[4096];
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error, 0, errorMsgBuffer, sizeof(errorMsgBuffer), nullptr);
            return errorMsgBuffer;
        };
        LOG_WARN("Failed to set window to always-on-top: %d/%s", GetLastError(), GetLastErrorString());
    }
    ShowWindow(SW_SHOW);

    // We don't need to call SetUpDX explicitly, because it will be called
    // on resize and that happens immediately when the window is created.

    // TODO: Load existing lyrics!
}

void ExternalLyricWindow::SetUpDX(bool force)
{
    TIME_FUNCTION();

    CRect rect = {};
    GetClientRect(&rect);
    if(m_d2d_bitmap != nullptr)
    {
        const D2D1_SIZE_U current_size = m_d2d_bitmap->GetPixelSize();
        const bool size_changed = (m_d2d_bitmap == nullptr)
            || (current_size.width != UINT(rect.Width()))
            || (current_size.height != UINT(rect.Height()));
        const bool is_empty = ((rect.Width() == 0) || (rect.Height() == 0));
        if(!force && (!size_changed || is_empty))
        {
            LOG_INFO("DirectX target size has not changed and setup was not forced, skipping setup...");
            return;
        }
    }

    m_swap_chain.Reset();
    m_d3d_device.Reset();
    m_d2d_device.Reset();
    m_d2d_dc.Reset();
    m_d2d_bitmap.Reset();
    m_dcomp_device.Reset();
    m_dcomp_target.Reset();
    m_dcomp_visual.Reset();

    // Approach to pixel-perfect window transparency adapted from: https://learn.microsoft.com/en-us/archive/msdn-magazine/2014/june/windows-with-c-high-performance-window-layering-using-the-windows-composition-engine
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef _NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_DRIVER_TYPE driver_types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    for(D3D_DRIVER_TYPE driver_type : driver_types)
    {
        HRESULT result = D3D11CreateDevice(nullptr,
                                           driver_type,
                                           nullptr,
                                           flags,
                                           levels,
                                           sizeof(levels)/sizeof(levels[0]),
                                           D3D11_SDK_VERSION,
                                           &m_d3d_device,
                                           nullptr,
                                           nullptr);
        if((result == S_OK) && (m_d3d_device.Get() != nullptr))
        {
            LOG_INFO("Successfully created D3D device using driver type %d", int(driver_type));
            break;
        }
        else
        {
            LOG_WARN("Failed to create D3D device using driver type %d: %u", int(driver_type), uint32_t(result));
        }
    }
    if(m_d3d_device == nullptr)
    {
        LOG_ERROR("Failed to create D3D device after trying all driver types. Unable to draw lyric panel");
        return;
    }

    // Setup our Direct2D device
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device = nullptr;
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> dxgi_adapter = nullptr;
        Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory = nullptr;
        if(HR_SUCCESS(m_d3d_device.As(&dxgi_device)))
        {
            if(HR_SUCCESS(dxgi_device->GetParent(IID_IDXGIAdapter, &dxgi_adapter)))
            {
                HR_SUCCESS(dxgi_adapter->GetParent(IID_IDXGIFactory2, &dxgi_factory));
            }
        }
        if(dxgi_factory == nullptr)
        {
            LOG_ERROR("Failed to create DXGI factory. Unable to draw lyric panel");
            return;
        }

        DXGI_SWAP_CHAIN_DESC1 description = {};
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SampleDesc.Count = 1;
        description.Width  = UINT(rect.Width());
        description.Height = UINT(rect.Height());

        if((description.Width == 0) || (description.Height == 0))
        {
            LOG_INFO("Attempt to create a swapchain with zero area. The panel was probably minimised. Drawing will be disabled");
            return;
        }

        if(m_direct_composition == nullptr)
        {
            description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            if(!HR_SUCCESS(dxgi_factory->CreateSwapChainForHwnd(
                            dxgi_device.Get(),
                            m_hWnd,
                            &description,
                            nullptr,
                            nullptr,
                            m_swap_chain.GetAddressOf()
                            )))
            {
                LOG_ERROR("Failed to create DirectX swap chain for window. Unable to draw lyric panel");
                return;
            }
        }
        else
        {
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if(!HR_SUCCESS(dxgi_factory->CreateSwapChainForComposition(
                            dxgi_device.Get(),
                            &description,
                            nullptr,
                            m_swap_chain.GetAddressOf())))
            {
                LOG_ERROR("Failed to create DirectX swap chain for composition. Unable to draw lyric panel");
                return;
            }
        }
    }

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory = nullptr;
    const D2D1_FACTORY_OPTIONS options = { D2D1_DEBUG_LEVEL_INFORMATION };
    if(HR_SUCCESS(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    options,
                                    d2d_factory.GetAddressOf())))
    {
        HR_SUCCESS(d2d_factory->CreateDevice(dxgi_device.Get(), m_d2d_device.GetAddressOf()));
    }
    if(m_d2d_device == nullptr)
    {
        LOG_ERROR("Failed to create D2D device. Unable to draw lyric panel");
        return;
    }

    bool success = true;
    if(m_direct_composition != nullptr)
    {
        HRESULT (STDAPICALLTYPE *MyDCompositionCreateDevice)(IDXGIDevice *dxgiDevice, REFIID iid, void **dcompositionDevice) = nullptr;
        FARPROC dcomp_create_device_proc = GetProcAddress(m_direct_composition, "DCompositionCreateDevice");
        MyDCompositionCreateDevice = (HRESULT (STDAPICALLTYPE*)(IDXGIDevice *dxgiDevice, REFIID iid, void **dcompositionDevice))dcomp_create_device_proc;
        success = success && (MyDCompositionCreateDevice != nullptr);

        // Create DirectComposition device for composing the swapchain into our window
        success = success && HR_SUCCESS(MyDCompositionCreateDevice(dxgi_device.Get(),
                    __uuidof(m_dcomp_device),
                    (void **)m_dcomp_device.GetAddressOf()));
        success = success && HR_SUCCESS(m_dcomp_device->CreateTargetForHwnd(m_hWnd, true, m_dcomp_target.GetAddressOf()));
        success = success && HR_SUCCESS(m_dcomp_device->CreateVisual(m_dcomp_visual.GetAddressOf()));
        success = success && HR_SUCCESS(m_dcomp_visual->SetContent(m_swap_chain.Get()));
        success = success && HR_SUCCESS(m_dcomp_target->SetRoot(m_dcomp_visual.Get()));
        success = success && HR_SUCCESS(m_dcomp_device->Commit());
    }
    if(!success)
    {
        LOG_ERROR("Failed to setup the required DirectComposition infrastructure. Unable to draw the lyric panel");
        return;
    }

    if(HR_SUCCESS(m_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2d_dc.GetAddressOf())))
    {
        D2D1_BITMAP_PROPERTIES1 properties = {};
        if(m_direct_composition == nullptr)
        {
            properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        }
        else
        {
            properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        }
        properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        Microsoft::WRL::ComPtr<IDXGISurface2> surface = nullptr;
        success = success && HR_SUCCESS(m_swap_chain->GetBuffer(0, IID_IDXGISurface2, (void **)surface.GetAddressOf()));
        success = success && HR_SUCCESS(m_d2d_dc->CreateBitmapFromDxgiSurface(surface.Get(), properties, m_d2d_bitmap.GetAddressOf()));
        if(success)
        {
            m_d2d_dc->SetTarget(m_d2d_bitmap.Get());
        }
        else
        {
            LOG_ERROR("Failed to setup backing bitmap for D2D device surface. Unable to draw lyric panel");
            m_d2d_dc.Reset(); // Reset this so we can use it to detect a setup failure before drawing
            return;
        }
    }
    else
    {
        LOG_ERROR("Failed to create D2D device context. Unable to draw lyric panel");
        return;
    }
}

static int get_text_origin_y(D2D1_SIZE_F canvas_size, int font_ascent_px, int font_descent_px)
{
    // NOTE: The drawing call uses the glyph baseline as the origin.
    //       We want our text to be perfectly vertically centered, so we need to offset it
    //       but the difference between the baseline and the vertical centre of the font.
    const int baseline_centre_correction = (font_ascent_px - font_descent_px)/2;
    int top_y = baseline_centre_correction;

    switch(preferences::display::text_alignment())
    {
        case TextAlignment::MidCentre:
        case TextAlignment::MidLeft:
        case TextAlignment::MidRight:
            top_y += int(0.5f*canvas_size.height);
            break;

        case TextAlignment::TopCentre:
        case TextAlignment::TopLeft:
        case TextAlignment::TopRight:
            top_y += font_ascent_px;
            break;

        default:
            LOG_WARN("Unrecognised text alignment option");
            break;
    }

    return top_y;
}

static int _WrapSimpleLyricsLineToRect(D2DTextRenderContext& render, const D2D1_SIZE_F canvas_size, std::tstring_view line, int origin_y, bool draw_requested)
{
    const int line_height = render.font_ascent_px + render.font_descent_px + preferences::display::linegap();
    if(line.empty())
    {
        return line_height;
    }

    // Remove trailing whitespace
    // We do this once now (before allocating anything dependent on string length)
    // and then since we don't ever move the "end" of the string, we assume that line
    // doesn't end in a space for the rest of the function.
    size_t last_not_space = line.find_last_not_of(_T(' '));
    if(last_not_space == std::tstring_view::npos)
    {
        return line_height; // Our line is exclusively whitespace
    }
    else
    {
        size_t trailing_spaces = line.length() - 1 - last_not_space;
        line.remove_suffix(trailing_spaces);
    }

    bool success = true;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory = nullptr;
    success = success && HR_SUCCESS(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf())
        ));

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout = nullptr;
    success = success && HR_SUCCESS(dwrite_factory->CreateTextLayout(
            line.data(),
            line.length(),
            render.text_format,
            canvas_size.width,
            canvas_size.height,
            layout.GetAddressOf()
            ));

    DWRITE_TEXT_METRICS layout_metrics = {};
    success = success && HR_SUCCESS(layout->GetMetrics(&layout_metrics));

    if(draw_requested)
    {
        D2D1_POINT_2F origin = {0.0f, float(origin_y)};
        render.device->DrawTextLayout(
                origin,
                layout.Get(),
                render.brush,
                D2D1_DRAW_TEXT_OPTIONS_NO_SNAP
                );
    }

    return int(layout_metrics.lineCount) * line_height;
}

// Ordinarily a single "line" from the lyric data is just one row (pre-wrapping) of text.
// However if multiple lines have the exact same timestamp, they get combined and are presented
// here as a single "line" that contains newline chars.
// We refer to these here as simple & compound lines.
static int _WrapCompoundLyricsLineToRect(D2DTextRenderContext& render, D2D1_SIZE_F canvas_size, std::tstring_view line, int origin_y, bool draw_requested)
{
    if(line.length() == 0)
    {
        return _WrapSimpleLyricsLineToRect(render, canvas_size, line, origin_y, draw_requested);
    }

    const int original_origin_y = origin_y;
    size_t start_index = 0;
    while(start_index < line.length())
    {
        size_t end_index = min(line.length(), line.find('\n', start_index));
        size_t length = end_index - start_index;
        std::tstring_view view(&line.data()[start_index], length);
        const int row_height = _WrapSimpleLyricsLineToRect(render, canvas_size, view, origin_y, draw_requested);
        origin_y += row_height;
        start_index = end_index+1;
    }

    const int result = origin_y - original_origin_y;
    return result;
}
static int ComputeWrappedLyricLineHeight(D2DTextRenderContext& render, D2D1_SIZE_F canvas_size, const std::tstring_view line)
{
    return _WrapCompoundLyricsLineToRect(render, canvas_size, line, 0, false);
}

static int DrawWrappedLyricLine(D2DTextRenderContext& render, D2D1_SIZE_F canvas_size, const std::tstring_view line, int origin_y)
{
    return _WrapCompoundLyricsLineToRect(render, canvas_size, line, origin_y, true);
}

D2D1::ColorF colour_gdi2dx(COLORREF in)
{
    const float normalize_byte = 1.0f/255.0f;
    return D2D1::ColorF(
                float(GetRValue(in))*normalize_byte,
                float(GetGValue(in))*normalize_byte,
                float(GetBValue(in))*normalize_byte,
                1);
}

static bool is_text_top_aligned()
{
    switch(preferences::display::text_alignment())
    {
        case TextAlignment::TopCentre:
        case TextAlignment::TopLeft:
        case TextAlignment::TopRight:
            return true;

        case TextAlignment::MidCentre:
        case TextAlignment::MidLeft:
        case TextAlignment::MidRight:
            return false;

        default:
            LOG_WARN("Unrecognised text alignment option");
            return false;
    }
}

void ExternalLyricWindow::DrawNoLyrics(D2DTextRenderContext& render)
{
    if(m_now_playing == nullptr)
    {
        return;
    }

    const D2D1_SIZE_F canvas_size = render.device->GetSize();

    std::string artist = track_metadata(m_now_playing_info, "artist");
    std::string album = track_metadata(m_now_playing_info, "album");
    std::string title = track_metadata(m_now_playing_info, "title");

    int total_height = 0;
    std::tstring artist_line;
    std::tstring album_line;
    std::tstring title_line;
    if(!artist.empty())
    {
        artist_line = _T("Artist: ") + to_tstring(artist);
        total_height += ComputeWrappedLyricLineHeight(render, canvas_size, artist_line);
    }
    if(!album.empty())
    {
        album_line = _T("Album: ") + to_tstring(album);
        total_height += ComputeWrappedLyricLineHeight(render, canvas_size, album_line);
    }
    if(!title.empty())
    {
        title_line = _T("Title: ") + to_tstring(title);
        total_height += ComputeWrappedLyricLineHeight(render, canvas_size, title_line);
    }

    int origin_y = get_text_origin_y(canvas_size, render.font_ascent_px, render.font_descent_px);
    if(!is_text_top_aligned())
    {
        origin_y -= total_height/2;
    }

    if(!artist_line.empty())
    {
        origin_y += DrawWrappedLyricLine(render, canvas_size, artist_line, origin_y);
    }
    if(!album_line.empty())
    {
        origin_y += DrawWrappedLyricLine(render, canvas_size, album_line, origin_y);
    }
    if(!title_line.empty())
    {
        origin_y += DrawWrappedLyricLine(render, canvas_size, title_line, origin_y);
    }

    std::optional<std::string> progress_msg = LyricUpdateQueue::get_progress_message();
    if(progress_msg.has_value())
    {
        std::tstring progress_text = to_tstring(progress_msg.value());
        origin_y += DrawWrappedLyricLine(render, canvas_size, progress_text, origin_y);
    }
}

void ExternalLyricWindow::DrawUntimedLyrics(LyricData& lyrics, D2DTextRenderContext& render)
{
    TIME_FUNCTION();
    double track_fraction = 0.0;
    if(preferences::display::scroll_type() == LineScrollType::Automatic)
    {
        const PlaybackTimeInfo playback_time = get_playback_time();
        track_fraction = playback_time.current_time / playback_time.track_length;
    }

    const D2D1_SIZE_F canvas_size = render.device->GetSize();

    COLORREF text_color = preferences::display::main_text_colour();
    render.brush->SetColor(colour_gdi2dx(text_color));

    const int total_height = std::accumulate(lyrics.lines.begin(), lyrics.lines.end(), 0,
        [&render, canvas_size](int x, const LyricDataLine& line)
        {
            return x + ComputeWrappedLyricLineHeight(render, canvas_size, line.text);
        });
    const int total_scrollable_height = total_height - (render.font_ascent_px + render.font_descent_px) - preferences::display::linegap();

    int origin_y = get_text_origin_y(canvas_size, render.font_ascent_px, render.font_descent_px);
    origin_y -= int(track_fraction * total_scrollable_height);

    for(const LyricDataLine& line : lyrics.lines)
    {
        int wrapped_line_height = DrawWrappedLyricLine(render, canvas_size, line.text, origin_y);
        if(wrapped_line_height <= 0)
        {
            LOG_WARN("Failed to draw unsynced text: %d", GetLastError());
            break;
        }
        origin_y += wrapped_line_height;
    }
}

struct LyricScrollPosition
{
    int active_line_index;
    double next_line_scroll_factor; // How far away from the active line (and towards the next line) we should be scrolled. Values are in the range [0,1]
};

static LyricScrollPosition get_scroll_position(const LyricData& lyrics, double current_time, double scroll_duration)
{
    int active_line_index = -1;
    int lyric_line_count = static_cast<int>(lyrics.lines.size());
    while((active_line_index+1 < lyric_line_count) && (current_time > lyrics.LineTimestamp(active_line_index+1)))
    {
        active_line_index++;
    }

    const double active_line_time = lyrics.LineTimestamp(active_line_index);
    const double next_line_time = lyrics.LineTimestamp(active_line_index+1);

    const double scroll_start_time = max(active_line_time, next_line_time - scroll_duration);
    const double scroll_end_time = next_line_time;

    double next_line_scroll_factor = lerp_inverse_clamped(scroll_start_time, scroll_end_time, current_time);
    return {active_line_index, next_line_scroll_factor};
}

void ExternalLyricWindow::DrawTimestampedLyrics(D2DTextRenderContext& render)
{
    const D2D1_SIZE_F canvas_size = render.device->GetSize();

    t_ui_color past_text_colour = preferences::display::past_text_colour();
    t_ui_color main_text_colour = preferences::display::main_text_colour();
    t_ui_color hl_colour = preferences::display::highlight_colour();

    const PlaybackTimeInfo playback_time = get_playback_time();
    const double scroll_time = preferences::display::scroll_time_seconds();
    const LyricScrollPosition scroll = get_scroll_position(m_lyrics, playback_time.current_time, scroll_time);

    const double fade_duration = preferences::display::highlight_fade_seconds();
    const LyricScrollPosition fade = get_scroll_position(m_lyrics, playback_time.current_time, fade_duration);

    int text_height_above_active_line = 0;
    int active_line_height = 0;
    if(scroll.active_line_index >= 0)
    {
        for(int i=0; i<scroll.active_line_index; i++)
        {
            text_height_above_active_line += ComputeWrappedLyricLineHeight(render, canvas_size, m_lyrics.lines[i].text);
        }
        active_line_height = ComputeWrappedLyricLineHeight(render, canvas_size, m_lyrics.lines[scroll.active_line_index].text);
    }

    int next_line_scroll = (int)((double)active_line_height * scroll.next_line_scroll_factor);
    int origin_y = get_text_origin_y(canvas_size, render.font_ascent_px, render.font_descent_px);
    origin_y -= text_height_above_active_line + next_line_scroll;

    const int lyric_line_count = static_cast<int>(m_lyrics.lines.size());
    for(int line_index=0; line_index < lyric_line_count; line_index++)
    {
        const LyricDataLine& line = m_lyrics.lines[line_index];
        if(line_index == scroll.active_line_index)
        {
            t_ui_color colour = lerp(hl_colour, past_text_colour, fade.next_line_scroll_factor);
            render.brush->SetColor(colour_gdi2dx(colour));
        }
        else if(line_index == scroll.active_line_index+1)
        {
            t_ui_color colour = lerp(main_text_colour, hl_colour, fade.next_line_scroll_factor);
            render.brush->SetColor(colour_gdi2dx(colour));
        }
        else if(line_index < scroll.active_line_index)
        {
            render.brush->SetColor(colour_gdi2dx(past_text_colour));
        }
        else
        {
            render.brush->SetColor(colour_gdi2dx(main_text_colour));
        }

        int wrapped_line_height = DrawWrappedLyricLine(render, canvas_size, line.text, origin_y);
        if(wrapped_line_height == 0)
        {
            LOG_ERROR("Failed to draw synced text");
            StopTimer();
            break;
        }

        origin_y += wrapped_line_height;
    }
}


void ExternalLyricWindow::OnWindowDestroy()
{
    LyricPanel::OnWindowDestroy();
    if(!core_api::is_shutting_down())
    {
        cfg_external_window_was_open = 0;
    }

    m_swap_chain = nullptr;
    m_d3d_device = nullptr;
    m_d2d_device = nullptr;
    m_d2d_dc = nullptr;
    m_d2d_bitmap = nullptr;
    m_d2d_albumart_bitmap = nullptr;
    m_dcomp_device = nullptr;
    m_dcomp_target = nullptr;
    m_dcomp_visual = nullptr;
}

LRESULT ExternalLyricWindow::OnWindowCreate(LPCREATESTRUCT params)
{
    cfg_external_window_was_open = 1;

    SetUpDX(false); // TODO: This is kinda silly because we're going to immediately call this again after we return in the on_resize callback
    return LyricPanel::OnWindowCreate(params);
}

void ExternalLyricWindow::OnWindowMove(CPoint new_origin)
{
    LyricPanel::OnWindowMove(new_origin);
    cfg_external_window_previous_x = new_origin.x;
    cfg_external_window_previous_y = new_origin.y;
}

void ExternalLyricWindow::OnWindowResize(UINT request_type, CSize new_size)
{
    SetUpDX(false);
    LyricPanel::OnWindowResize(request_type, new_size);
    Invalidate();

    cfg_external_window_previous_size_x = new_size.cx;
    cfg_external_window_previous_size_y = new_size.cy;
}

void ExternalLyricWindow::OnPaint(CDCHandle)
{
    // Tell GDI that we've redrawn the window.
    // We do this here because even if drawing fails below, we don't want to keep getting
    // called to redraw. It failed once it will almost certainly fail next time too.
    ValidateRect(nullptr);

    if(IsIconic())
    {
        // The window is minimized, don't bother drawing
        return;
    }

    if(m_search_pending)
    {
        m_search_pending = false;

        // We need to check that there is a now-playing track still.
        // There might not be one if a new track started while fb2k was minimised (so we don't repaint) and then playback stopped before fb2k got maximised again.
        // In that case we'd previously try to use m_now_playing to power the search & search-avoidance and would crash.
        if(m_now_playing != nullptr)
        {
            // NOTE: We also track a generation counter that increments every time you change the search config
            //       so that if you don't find lyrics with some active sources and then add more, it'll search
            //       again at least once, possibly finding something if there are new active sources.
            if(search_avoidance_allows_search(m_now_playing))
            {
                if(should_panel_search(this))
                {
                    InitiateLyricSearch();
                }
            }
            else
            {
                LOG_INFO("Skipped search because it's expected to fail anyway and was not specifically requested");
                m_lyrics = {};
            }
        }
    }

    if(should_panel_search(this))
    {
        LyricUpdateQueue::check_for_available_updates();
    }

    if(m_d2d_dc == nullptr)
    {
        return;
    }
    D2DTextRenderContext render = {};
    render.device = m_d2d_dc.Get();

    LOGFONT logfont = {};
    const int font_bytes = GetObject(preferences::display::font(), sizeof(logfont), &logfont);

    bool success = true;
    Microsoft::WRL::ComPtr<IDWriteFontFace1> fontface = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush = nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format = nullptr;
    if(font_bytes == 0)
    {
        LOG_ERROR("Failed to get configured logical font spec");
        success = false;
    }
    else
    {
        // If we upgraded our minimum OS version to Windows 10 then we could replace
        // this with GetDpiForWindow() which doesn't need an HDC.
        HDC dc = GetDC();
        const float device_dpi = float(GetDeviceCaps(dc, LOGPIXELSY));
        ReleaseDC(dc);

        const float font_point_size = -72.0f * float(logfont.lfHeight)/device_dpi; // See https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-logfontw
        render.pixels_per_em = font_point_size * device_dpi * (1.0f/72.0f);

        Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory = nullptr;
        IDWriteGdiInterop* gdi_interop = nullptr; // We don't create this, so don't destroy it
        Microsoft::WRL::ComPtr<IDWriteFont> dwrite_font = nullptr;
        Microsoft::WRL::ComPtr<IDWriteFontFace> dwrite_fontface_0 = nullptr;
        success = success && HR_SUCCESS(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf())
            ));
        success = success && HR_SUCCESS(dwrite_factory->GetGdiInterop(&gdi_interop));
        success = success && HR_SUCCESS(gdi_interop->CreateFontFromLOGFONT(&logfont, dwrite_font.GetAddressOf()));
        success = success && HR_SUCCESS(dwrite_font->CreateFontFace(dwrite_fontface_0.GetAddressOf()));
        success = success && HR_SUCCESS(dwrite_fontface_0.As(&fontface));
        render.fontface = fontface.Get();
        if(fontface != nullptr)
        {
            // See https://learn.microsoft.com/en-us/windows/win32/gdi/device-vs--design-units
            DWRITE_FONT_METRICS font_metrics = {}; // NOTE: These are all in "font design units", so we need to convert them to pixels here
            fontface->GetMetrics(&font_metrics);
            render.pixels_per_design_unit = render.pixels_per_em/(float(font_metrics.designUnitsPerEm));
            render.font_ascent_px = int(font_metrics.ascent * render.pixels_per_design_unit);
            render.font_descent_px = int(font_metrics.descent * render.pixels_per_design_unit);
        }

        success = success && HR_SUCCESS(m_d2d_dc->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1), brush.GetAddressOf()));
        render.brush = brush.Get();

        WCHAR locale_name[LOCALE_NAME_MAX_LENGTH] = {};
        int locale_name_length = GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH);
        success = success && (locale_name_length > 0);

        IDWriteFontCollection* system_font_collection = nullptr;
        success = success && HR_SUCCESS(dwrite_factory->GetSystemFontCollection(&system_font_collection, false));

        IDWriteFontFamily* font_family = nullptr;
        success = success && HR_SUCCESS(dwrite_font->GetFontFamily(&font_family));

        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> font_family_names = nullptr;
        success = success && HR_SUCCESS(font_family->GetFamilyNames(font_family_names.GetAddressOf()));

        UINT32 locale_index = 0;
        BOOL locale_found = false;
        success = success && HR_SUCCESS(font_family_names->FindLocaleName(locale_name, &locale_index, &locale_found));

        if(!locale_found)
        {
            success = success && HR_SUCCESS(font_family_names->FindLocaleName(_T("en-us"), &locale_index, &locale_found));
            if(!locale_found)
            {
                LOG_WARN("Failed to find appropriate font locale");
            }
        }

        WCHAR font_family_name[1024] = {};
        success = success && HR_SUCCESS(font_family_names->GetString(locale_index, font_family_name, 1024));

        success = success && HR_SUCCESS(dwrite_factory->CreateTextFormat(
                    font_family_name,
                    system_font_collection,
                    dwrite_font->GetWeight(),
                    dwrite_font->GetStyle(),
                    dwrite_font->GetStretch(),
                    -float(logfont.lfHeight),
                    locale_name,
                    text_format.GetAddressOf()
                    ));

        switch(preferences::display::text_alignment())
        {
            case TextAlignment::MidCentre:
            case TextAlignment::TopCentre:
                success = success && HR_SUCCESS(text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
                break;

            case TextAlignment::MidLeft:
            case TextAlignment::TopLeft:
                success = success && HR_SUCCESS(text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING));
                break;

            case TextAlignment::MidRight:
            case TextAlignment::TopRight:
                success = success && HR_SUCCESS(text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING));
                break;

            default:
                LOG_WARN("Unrecognised text alignment option");
                break;
        }

        render.text_format = text_format.Get();
    }

    if(success)
    {
        m_d2d_dc->BeginDraw();
        m_d2d_dc->Clear();

        if(m_d2d_albumart_bitmap != nullptr)
        {
            render.device->DrawBitmap(
                    m_d2d_albumart_bitmap.Get(),
                    nullptr, // rectangle,
                    preferences::background::external_window_opacity(),
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                    nullptr //source_rectangle
                    );
        }

        if(m_lyrics.IsEmpty())
        {
            DrawNoLyrics(render);
        }
        else if(m_lyrics.IsTimestamped() &&
                (preferences::display::scroll_type() == LineScrollType::Automatic))
        {
            DrawTimestampedLyrics(render);
        }
        else // We have lyrics, but no timestamps
        {
            DrawUntimedLyrics(m_lyrics, render);
        }

        HRESULT end_result = m_d2d_dc->EndDraw();
        if(end_result == D2DERR_RECREATE_TARGET)
        {
            LOG_INFO("Draw failed with a request to recreate the render target");
            SetUpDX(true);
        }
        else if(end_result != S_OK)
        {
            LOG_WARN("Failed to draw unsynced lyrics: %d", int(end_result));
            StopTimer();
        }

        const int sync = 1;
        const int flags = 0;
        if(!HR_SUCCESS(m_swap_chain->Present(sync, flags)))
        {
            LOG_WARN("DirectX Present failed, reinitializing...");
            SetUpDX(true);
        }
    }
}

bool ExternalLyricWindow::is_panel_ui_in_edit_mode()
{
    return false;
}

void ExternalLyricWindow::compute_background_image()
{
    LyricPanel::compute_background_image();
    m_d2d_albumart_bitmap.Reset();

    if(m_d2d_dc == nullptr)
    {
        LOG_INFO("No Direct2D context available, skipping background image recompute");
        return;
    }

    bool success = true;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory = nullptr;
    success = success && HR_SUCCESS(CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_IWICImagingFactory,
                    (void**)wic_factory.GetAddressOf()
                    ));

    Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap = nullptr;
    success = success && HR_SUCCESS(wic_factory->CreateBitmapFromMemory(
                    m_background_img.width,
                    m_background_img.height,
                    GUID_WICPixelFormat32bppBGR,
                    m_background_img.width * 4,
                    m_background_img.width * m_background_img.height * 4,
                    m_background_img.pixels,
                    wic_bitmap.GetAddressOf()
                    ));

    success = success && HR_SUCCESS(m_d2d_dc->CreateBitmapFromWicBitmap(
            wic_bitmap.Get(),
            m_d2d_albumart_bitmap.GetAddressOf()
            ));
}

void show_external_lyric_window()
{
    if(g_external_window == nullptr)
    {
        g_external_window = new ExternalLyricWindow();
    }
    if(!g_external_window->IsWindow())
    {
        g_external_window->SetUp();
    }
}

LyricPanel* get_external_lyric_window()
{
    if((g_external_window != nullptr)
        && g_external_window->IsWindow())
    {
        return g_external_window;
    }

    return nullptr;
}

class ExternalWindowLifetimeWarden : public initquit
{
    void on_init() final
    {
        const bool was_open = (cfg_external_window_was_open.get_value() != 0);
        if(was_open)
        {
            show_external_lyric_window();
        }
    }
    void on_quit() final
    {
        if(g_external_window != nullptr)
        {
            if(g_external_window->IsWindow())
            {
                g_external_window->DestroyWindow();
            }
            delete g_external_window;
            g_external_window = nullptr;
        }
    }
};
static initquit_factory_t<ExternalWindowLifetimeWarden> g_warden_factory;

