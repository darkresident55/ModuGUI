// dear imgui, ModuGUI fork
// [SECTION] Modularity: widget shading
//
// Everything behind ImGuiShadeTheme / ImGui::ShadeRect(). See the matching section in imgui.h
// for the concepts (class, state, inheritance); this file is the mechanics:
//   - ImGuiShadeParams / ImGuiShadeTheme defaults
//   - ShadeResolveTheme(): flatten inheritance once, so drawing is a plain array lookup
//   - ShadeRectEx(): fill + gradient + bevel + borders for one rectangle
//   - StyleShadeThemeFlat() / StyleShadeThemeDesktop(): the two built-in themes
//
// Rules this file plays by, because it runs for nearly every rectangle the editor draws:
//   - no allocations, no containers, no per-frame caches to invalidate
//   - the gradient re-colors the vertices AddRectFilled() already emitted, it does not add
//     geometry, so a gradient costs a handful of float ops per rect and zero extra draw calls
//   - thicknesses are snapped to whole pixels after DPI scaling, so a "1px" hairline stays a
//     crisp 1px hairline at 100% and a crisp 2px one at 200% instead of turning into mush

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_internal.h"

//-----------------------------------------------------------------------------
// [SECTION] Defaults
//-----------------------------------------------------------------------------

ImGuiShadeParams::ImGuiShadeParams()
{
    Flags = ImGuiShadeFlags_None;
    GradientTop = 0.0f;
    GradientBottom = 0.0f;
    BevelSize = 0.0f;
    BevelIntensity = 1.0f;
    BorderSize = -1.0f;             // < 0 = whatever ImGuiStyle says for this kind of rectangle
    ColTopHighlight = 0;
    ColBottomShadow = 0;
    ColInnerShadow = 0;
    ColInnerHighlight = 0;
    ColBorderTop = 0;
    ColBorderBottom = 0;
    ColBorderLeft = 0;
    ColBorderRight = 0;
    ColBorderAll = 0;
}

ImGuiShadeTheme::ImGuiShadeTheme()
{
    Enabled = false;                // Opt-in: an app that never calls SetShadeTheme() renders exactly as before
    GradientScale = 1.0f;
    BevelScale = 1.0f;
    Scale = 1.0f;
    // Params[][] are default-constructed by ImGuiShadeParams::ImGuiShadeParams().
}

//-----------------------------------------------------------------------------
// [SECTION] Color helpers
//-----------------------------------------------------------------------------

// Lighten (delta > 0) or darken (delta < 0) a packed color, keeping alpha.
// Additive rather than multiplicative on purpose: on the near-black fills a dark editor theme is
// made of, a multiplier barely moves the color, while a small additive delta reads as intended
// at both ends of the range.
ImU32 ImGui::ShadeAdjustColor(ImU32 col, float luminance_delta)
{
    if (luminance_delta == 0.0f)
        return col;
    const int d = (int)(luminance_delta * 255.0f + (luminance_delta > 0.0f ? 0.5f : -0.5f));
    const int r = ImClamp((int)((col >> IM_COL32_R_SHIFT) & 0xFF) + d, 0, 255);
    const int g = ImClamp((int)((col >> IM_COL32_G_SHIFT) & 0xFF) + d, 0, 255);
    const int b = ImClamp((int)((col >> IM_COL32_B_SHIFT) & 0xFF) + d, 0, 255);
    const ImU32 a = col & IM_COL32_A_MASK;
    return a | ((ImU32)r << IM_COL32_R_SHIFT) | ((ImU32)g << IM_COL32_G_SHIFT) | ((ImU32)b << IM_COL32_B_SHIFT);
}

static inline ImU32 ImShadeScaleAlpha(ImU32 col, float scale)
{
    if (scale >= 1.0f)
        return col;
    const int a = (int)(((col >> IM_COL32_A_SHIFT) & 0xFF) * ImMax(scale, 0.0f) + 0.5f);
    return (col & ~IM_COL32_A_MASK) | ((ImU32)ImClamp(a, 0, 255) << IM_COL32_A_SHIFT);
}

// DPI-aware hairline: scale, then snap to a whole pixel with a 1px floor.
static inline float ImShadeSnapThickness(float unscaled_size, float scale)
{
    if (unscaled_size <= 0.0f)
        return 0.0f;
    return ImMax(1.0f, IM_TRUNC(unscaled_size * ImMax(scale, 0.01f)));
}

//-----------------------------------------------------------------------------
// [SECTION] Inheritance
//-----------------------------------------------------------------------------

static void ImShadeMergeFromParent(ImGuiShadeParams& dst, const ImGuiShadeParams& parent)
{
    if (dst.Flags & ImGuiShadeFlags_InheritGradient)
    {
        dst.GradientTop = parent.GradientTop;
        dst.GradientBottom = parent.GradientBottom;
    }
    if (dst.Flags & ImGuiShadeFlags_InheritBevel)
    {
        dst.BevelSize = parent.BevelSize;
        dst.BevelIntensity = parent.BevelIntensity;
        dst.ColTopHighlight = parent.ColTopHighlight;
        dst.ColBottomShadow = parent.ColBottomShadow;
        dst.ColInnerShadow = parent.ColInnerShadow;
        dst.ColInnerHighlight = parent.ColInnerHighlight;
    }
    if (dst.Flags & ImGuiShadeFlags_InheritBorder)
    {
        dst.BorderSize = parent.BorderSize;
        dst.ColBorderTop = parent.ColBorderTop;
        dst.ColBorderBottom = parent.ColBorderBottom;
        dst.ColBorderLeft = parent.ColBorderLeft;
        dst.ColBorderRight = parent.ColBorderRight;
        dst.ColBorderAll = parent.ColBorderAll;
    }
}

// Flatten (class, state) -> (class, Normal) -> (Generic, Normal) -> built-in default.
// Done once per theme change so the draw path never walks a chain.
void ImGui::ShadeResolveTheme(const ImGuiShadeTheme& src, ImGuiShadeParams out[ImGuiShadeClass_COUNT][ImGuiShadeState_COUNT])
{
    const ImGuiShadeParams fallback;     // Built-in default: no gradient, no bevel, style-driven border

    // Root first: everything else can lean on it.
    ImGuiShadeParams root = src.Params[ImGuiShadeClass_Generic][ImGuiShadeState_Normal];
    if ((root.Flags & ImGuiShadeFlags_Set) == 0)
        root = fallback;
    else
        ImShadeMergeFromParent(root, fallback);

    for (int c = 0; c < ImGuiShadeClass_COUNT; c++)
    {
        // Per-class Normal entry, parented to the root.
        ImGuiShadeParams class_normal = src.Params[c][ImGuiShadeState_Normal];
        if ((class_normal.Flags & ImGuiShadeFlags_Set) == 0)
            class_normal = root;
        else
            ImShadeMergeFromParent(class_normal, root);
        out[c][ImGuiShadeState_Normal] = class_normal;

        for (int s = 1; s < ImGuiShadeState_COUNT; s++)
        {
            ImGuiShadeParams entry = src.Params[c][s];
            if ((entry.Flags & ImGuiShadeFlags_Set) == 0)
                entry = class_normal;
            else
                ImShadeMergeFromParent(entry, class_normal);
            out[c][s] = entry;
        }
    }
}

const ImGuiShadeTheme& ImGui::GetShadeTheme()
{
    ImGuiContext& g = *GImGui;
    return g.ShadeTheme;
}

void ImGui::SetShadeTheme(const ImGuiShadeTheme& theme)
{
    ImGuiContext& g = *GImGui;
    g.ShadeTheme = theme;
    ShadeResolveTheme(g.ShadeTheme, g.ShadeResolved);
}

const ImGuiShadeParams& ImGui::GetShadeParams(ImGuiShadeClass shade_class, ImGuiShadeState shade_state)
{
    ImGuiContext& g = *GImGui;
    IM_ASSERT(shade_class >= 0 && shade_class < ImGuiShadeClass_COUNT);
    IM_ASSERT(shade_state >= 0 && shade_state < ImGuiShadeState_COUNT);
    return g.ShadeResolved[shade_class][shade_state];
}

//-----------------------------------------------------------------------------
// [SECTION] Names
//-----------------------------------------------------------------------------
// Kept in sync with the enums by the asserts below. These strings end up in saved themes, so
// treat them as a file format: add to the end, never rename.

static const char* const GShadeClassNames[] =
{
    "Generic", "Window", "Child", "Popup", "TitleBar", "MenuBar", "Button", "Frame",
    "Header", "Tab", "TabActive", "Grab", "ScrollbarBg", "TableHeader", "Separator",
};
IM_STATIC_ASSERT(IM_ARRAYSIZE(GShadeClassNames) == ImGuiShadeClass_COUNT);

static const char* const GShadeStateNames[] =
{
    "Normal", "Hovered", "Active", "Selected", "Focused", "Disabled",
};
IM_STATIC_ASSERT(IM_ARRAYSIZE(GShadeStateNames) == ImGuiShadeState_COUNT);

const char* ImGui::GetShadeClassName(ImGuiShadeClass shade_class)
{
    IM_ASSERT(shade_class >= 0 && shade_class < ImGuiShadeClass_COUNT);
    return GShadeClassNames[shade_class];
}

const char* ImGui::GetShadeStateName(ImGuiShadeState shade_state)
{
    IM_ASSERT(shade_state >= 0 && shade_state < ImGuiShadeState_COUNT);
    return GShadeStateNames[shade_state];
}

ImGuiShadeClass ImGui::FindShadeClassByName(const char* name)
{
    if (name == NULL)
        return -1;
    for (int i = 0; i < ImGuiShadeClass_COUNT; i++)
        if (strcmp(GShadeClassNames[i], name) == 0)
            return i;
    return -1;
}

ImGuiShadeState ImGui::FindShadeStateByName(const char* name)
{
    if (name == NULL)
        return -1;
    for (int i = 0; i < ImGuiShadeState_COUNT; i++)
        if (strcmp(GShadeStateNames[i], name) == 0)
            return i;
    return -1;
}

ImGuiShadeState ImGui::ShadeStateFromInteraction(bool hovered, bool held, bool selected)
{
    ImGuiContext& g = *GImGui;
    if (g.CurrentItemFlags & ImGuiItemFlags_Disabled)
        return ImGuiShadeState_Disabled;
    if (held)
        return ImGuiShadeState_Active;
    if (hovered)
        return ImGuiShadeState_Hovered;
    if (selected)
        return ImGuiShadeState_Selected;
    return ImGuiShadeState_Normal;
}

//-----------------------------------------------------------------------------
// [SECTION] Drawing
//-----------------------------------------------------------------------------

// Horizontal hairline, inset past the corner radius so it never crosses a rounded corner.
static inline void ImShadeHLine(ImDrawList* draw_list, float x1, float x2, float y, float rounding, ImU32 col, float thickness)
{
    if ((col & IM_COL32_A_MASK) == 0)
        return;
    const float inset = ImMax(rounding * 0.60f, 0.0f);
    x1 += inset;
    x2 -= inset;
    if (x2 <= x1)
        return;
    draw_list->AddLine(ImVec2(x1, y), ImVec2(x2, y), col, thickness);
}

static inline void ImShadeVLine(ImDrawList* draw_list, float x, float y1, float y2, float rounding, ImU32 col, float thickness)
{
    if ((col & IM_COL32_A_MASK) == 0)
        return;
    const float inset = ImMax(rounding * 0.60f, 0.0f);
    y1 += inset;
    y2 -= inset;
    if (y2 <= y1)
        return;
    draw_list->AddLine(ImVec2(x, y1), ImVec2(x, y2), col, thickness);
}

void ImGui::ShadeRectEx(ImDrawList* draw_list, const ImVec2& p_min, const ImVec2& p_max, ImU32 fill_col, const ImGuiShadeParams& params, float rounding, ImDrawFlags draw_flags)
{
    ImGuiContext& g = *GImGui;
    const ImGuiShadeTheme& theme = g.ShadeTheme;

    const float w = p_max.x - p_min.x;
    const float h = p_max.y - p_min.y;
    if (w <= 0.0f || h <= 0.0f)
        return;

    // --- Fill (+ vertical gradient) ---
    const bool want_gradient = theme.Enabled
        && (params.Flags & ImGuiShadeFlags_NoGradient) == 0
        && (params.GradientTop != 0.0f || params.GradientBottom != 0.0f)
        && theme.GradientScale > 0.0f
        && (fill_col & IM_COL32_A_MASK) != 0;

    const int vtx_begin = draw_list->VtxBuffer.Size;
    draw_list->AddRectFilled(p_min, p_max, fill_col, rounding, draw_flags);
    if (want_gradient && draw_list->VtxBuffer.Size > vtx_begin)
    {
        // Recolor the vertices AddRectFilled() just wrote instead of emitting a second rect.
        // KeepAlpha leaves the anti-aliasing fringe intact, so rounded corners stay smooth.
        const ImU32 col_top = ShadeAdjustColor(fill_col, params.GradientTop * theme.GradientScale);
        const ImU32 col_bot = ShadeAdjustColor(fill_col, params.GradientBottom * theme.GradientScale);
        if (col_top != col_bot)
            ShadeVertsLinearColorGradientKeepAlpha(draw_list, vtx_begin, draw_list->VtxBuffer.Size, p_min, ImVec2(p_min.x, p_max.y), col_top, col_bot);
    }

    if (!theme.Enabled)
        return;

    // --- Bevel ---
    // Two hairlines just inside the top and bottom edges. Raised controls get light-on-top,
    // recessed ones get the inverse, which is the whole trick behind "button vs input field".
    const float bevel_thickness = ImShadeSnapThickness(params.BevelSize, theme.Scale);
    const float bevel_intensity = ImClamp(params.BevelIntensity * theme.BevelScale, 0.0f, 4.0f);
    if ((params.Flags & ImGuiShadeFlags_NoBevel) == 0 && bevel_thickness > 0.0f && bevel_intensity > 0.0f && h > bevel_thickness * 2.0f)
    {
        const bool recessed = (params.Flags & ImGuiShadeFlags_Recessed) != 0;
        // Derived defaults: a white wash for the lit edge, a black one for the shaded edge.
        // Both are alpha blends, so they read correctly over any fill color the theme picks.
        const ImU32 derived_light = ImShadeScaleAlpha(IM_COL32(255, 255, 255, 38), bevel_intensity);
        const ImU32 derived_dark = ImShadeScaleAlpha(IM_COL32(0, 0, 0, 56), bevel_intensity);

        ImU32 col_top = params.ColTopHighlight;
        ImU32 col_bottom = params.ColBottomShadow;
        if (col_top == 0)
            col_top = recessed ? derived_dark : derived_light;
        else
            col_top = ImShadeScaleAlpha(col_top, bevel_intensity);
        if (col_bottom == 0)
            col_bottom = recessed ? derived_light : derived_dark;
        else
            col_bottom = ImShadeScaleAlpha(col_bottom, bevel_intensity);

        const float half = bevel_thickness * 0.5f;
        const float y_top = IM_ROUND(p_min.y) + half;
        const float y_bottom = IM_ROUND(p_max.y) - half;
        ImShadeHLine(draw_list, p_min.x, p_max.x, y_top, rounding, col_top, bevel_thickness);
        ImShadeHLine(draw_list, p_min.x, p_max.x, y_bottom, rounding, col_bottom, bevel_thickness);

        // Optional second pair, one row further in. Only drawn when the theme asks for it:
        // this is the "deep" inset used for panel interiors and text fields.
        if (h > bevel_thickness * 4.0f)
        {
            if (params.ColInnerShadow != 0)
                ImShadeHLine(draw_list, p_min.x, p_max.x, y_top + bevel_thickness, rounding, ImShadeScaleAlpha(params.ColInnerShadow, bevel_intensity), bevel_thickness);
            if (params.ColInnerHighlight != 0)
                ImShadeHLine(draw_list, p_min.x, p_max.x, y_bottom - bevel_thickness, rounding, ImShadeScaleAlpha(params.ColInnerHighlight, bevel_intensity), bevel_thickness);
        }
    }

    // --- Borders ---
    // Per-edge colors are for square chrome (dock separators, toolbars, tab strips). When every
    // edge shares a color we take the cheap path and stroke a single rounded rect.
    const float border_thickness = ImShadeSnapThickness(params.BorderSize, theme.Scale);
    if (params.BorderSize > 0.0f && border_thickness > 0.0f)
    {
        const ImU32 col_all = params.ColBorderAll ? params.ColBorderAll : GetColorU32(ImGuiCol_Border);
        const ImU32 col_t = params.ColBorderTop ? params.ColBorderTop : col_all;
        const ImU32 col_b = params.ColBorderBottom ? params.ColBorderBottom : col_all;
        const ImU32 col_l = params.ColBorderLeft ? params.ColBorderLeft : col_all;
        const ImU32 col_r = params.ColBorderRight ? params.ColBorderRight : col_all;
        if (col_t == col_b && col_t == col_l && col_t == col_r)
        {
            if ((col_t & IM_COL32_A_MASK) != 0)
                draw_list->AddRect(p_min, p_max, col_t, rounding, draw_flags, border_thickness);
        }
        else
        {
            const float half = border_thickness * 0.5f;
            ImShadeHLine(draw_list, p_min.x, p_max.x, p_min.y + half, rounding, col_t, border_thickness);
            ImShadeHLine(draw_list, p_min.x, p_max.x, p_max.y - half, rounding, col_b, border_thickness);
            ImShadeVLine(draw_list, p_min.x + half, p_min.y, p_max.y, rounding, col_l, border_thickness);
            ImShadeVLine(draw_list, p_max.x - half, p_min.y, p_max.y, rounding, col_r, border_thickness);
        }
    }
}

void ImGui::ShadeRect(ImDrawList* draw_list, const ImVec2& p_min, const ImVec2& p_max, ImU32 fill_col, ImGuiShadeClass shade_class, ImGuiShadeState shade_state, float rounding, ImDrawFlags draw_flags)
{
    ShadeRectEx(draw_list, p_min, p_max, fill_col, GetShadeParams(shade_class, shade_state), rounding, draw_flags);
}

// RenderFrame() with a class attached. Keeps RenderFrame()'s border behavior (which is driven by
// ImGuiStyle::FrameBorderSize) so widgets that pass borders = true look the same under a theme
// that defines no border of its own.
void ImGui::RenderFrameShaded(ImVec2 p_min, ImVec2 p_max, ImU32 fill_col, bool borders, float rounding, ImGuiShadeClass shade_class, ImGuiShadeState shade_state, ImDrawFlags draw_flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    const ImGuiShadeParams& resolved = GetShadeParams(shade_class, shade_state);

    // The common case draws straight from the resolved entry. Only a caller that asked for no
    // outline while the theme defines one needs a patched copy, which is the rare path.
    const bool suppress_border = !borders && resolved.BorderSize > 0.0f;
    if (suppress_border)
    {
        ImGuiShadeParams params = resolved;
        params.BorderSize = -1.0f;
        ShadeRectEx(window->DrawList, p_min, p_max, fill_col, params, rounding, draw_flags);
    }
    else
    {
        ShadeRectEx(window->DrawList, p_min, p_max, fill_col, resolved, rounding, draw_flags);
    }

    // The theme's own border already ran inside ShadeRectEx().
    if (!suppress_border && resolved.BorderSize > 0.0f && g.ShadeTheme.Enabled)
        return;
    const float border_size = g.Style.FrameBorderSize;
    if (borders && border_size > 0.0f)
    {
        window->DrawList->AddRect(p_min + ImVec2(1, 1), p_max + ImVec2(1, 1), GetColorU32(ImGuiCol_BorderShadow), rounding, draw_flags, border_size);
        window->DrawList->AddRect(p_min, p_max, GetColorU32(ImGuiCol_Border), rounding, draw_flags, border_size);
    }
}

//-----------------------------------------------------------------------------
// [SECTION] Built-in themes
//-----------------------------------------------------------------------------

void ImGui::StyleShadeThemeFlat(ImGuiShadeTheme* dst)
{
    IM_ASSERT(dst != NULL);
    *dst = ImGuiShadeTheme();
}

// The shaded desktop-editor look: subtle vertical gradients, raised buttons/tabs/grabs, recessed
// input frames and panel interiors, restrained 1px edges. Colors are all derived from the fill,
// so this theme works on top of any palette; the Modularity default theme layers its own purple
// tints on top of it (see applyModularityShadeTheme() in EditorUI.cpp).
void ImGui::StyleShadeThemeDesktop(ImGuiShadeTheme* dst)
{
    IM_ASSERT(dst != NULL);
    ImGuiShadeTheme& t = *dst;
    t = ImGuiShadeTheme();
    t.Enabled = true;
    t.GradientScale = 1.0f;
    t.BevelScale = 1.0f;
    t.Scale = 1.0f;

    ImGuiShadeParams* p;

    // Generic: the root every other entry falls back to. Deliberately almost flat, because it
    // also covers rectangles ModuGUI cannot classify (RenderFrame() called from app code).
    p = &t.Params[ImGuiShadeClass_Generic][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.018f;
    p->GradientBottom = -0.018f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.55f;

    // Window body: a long, very shallow gradient. Anything stronger banding-artifacts over the
    // height of a maximized dock node.
    p = &t.Params[ImGuiShadeClass_Window][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    p->GradientTop = 0.014f;
    p->GradientBottom = -0.010f;

    // Panel interior: recessed, so contents read as sitting *inside* the window.
    p = &t.Params[ImGuiShadeClass_Child][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed;
    p->GradientTop = -0.014f;
    p->GradientBottom = 0.008f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.65f;

    // Popups and context menus float above everything: raised, slightly brighter at the top.
    p = &t.Params[ImGuiShadeClass_Popup][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.022f;
    p->GradientBottom = -0.014f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.8f;

    p = &t.Params[ImGuiShadeClass_TitleBar][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.030f;
    p->GradientBottom = -0.024f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.7f;
    p = &t.Params[ImGuiShadeClass_TitleBar][ImGuiShadeState_Focused];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.038f;
    p->GradientBottom = -0.028f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.0f;

    // Menu bars and toolbars: raised strip with a defined bottom edge separating them from the
    // content below. That bottom edge is most of what makes an editor read as "banded".
    p = &t.Params[ImGuiShadeClass_MenuBar][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.030f;
    p->GradientBottom = -0.026f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.85f;
    p->ColBottomShadow = IM_COL32(0, 0, 0, 110);

    // Buttons: the reference "raised" control.
    p = &t.Params[ImGuiShadeClass_Button][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.045f;
    p->GradientBottom = -0.040f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.0f;
    p = &t.Params[ImGuiShadeClass_Button][ImGuiShadeState_Hovered];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.055f;
    p->GradientBottom = -0.040f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.25f;
    // Pressed: the bevel flips and the gradient inverts. Same fill, reads as pushed in.
    p = &t.Params[ImGuiShadeClass_Button][ImGuiShadeState_Active];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = -0.040f;
    p->GradientBottom = 0.022f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.1f;
    // Disabled: flat. Depth implies "you can press this", so a disabled control should not have it.
    p = &t.Params[ImGuiShadeClass_Button][ImGuiShadeState_Disabled];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.010f;
    p->GradientBottom = -0.010f;

    // Input frames: the reference "recessed" control.
    p = &t.Params[ImGuiShadeClass_Frame][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed;
    p->GradientTop = -0.030f;
    p->GradientBottom = 0.016f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.0f;
    p = &t.Params[ImGuiShadeClass_Frame][ImGuiShadeState_Hovered];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed | ImGuiShadeFlags_InheritGradient | ImGuiShadeFlags_InheritBorder;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.2f;
    p = &t.Params[ImGuiShadeClass_Frame][ImGuiShadeState_Disabled];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = -0.008f;
    p->GradientBottom = 0.004f;

    // Selection rows: nearly flat, they are already carried by their fill color. A row that is
    // beveled as hard as a button turns a hierarchy into a wall of stripes.
    p = &t.Params[ImGuiShadeClass_Header][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    p->GradientTop = 0.020f;
    p->GradientBottom = -0.016f;
    p = &t.Params[ImGuiShadeClass_Header][ImGuiShadeState_Hovered];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.026f;
    p->GradientBottom = -0.018f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.5f;
    p = &t.Params[ImGuiShadeClass_Header][ImGuiShadeState_Active];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed;
    p->GradientTop = -0.024f;
    p->GradientBottom = 0.014f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.6f;

    // Inactive tab: sits below the active one, so it is shaded slightly *into* the bar.
    p = &t.Params[ImGuiShadeClass_Tab][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.016f;
    p->GradientBottom = -0.030f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.45f;
    p = &t.Params[ImGuiShadeClass_Tab][ImGuiShadeState_Hovered];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.040f;
    p->GradientBottom = -0.020f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.9f;
    p = &t.Params[ImGuiShadeClass_Tab][ImGuiShadeState_Disabled];    // Tab in an unfocused tab bar
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    p->GradientTop = 0.010f;
    p->GradientBottom = -0.018f;

    // Active tab: raised, brighter at the top, no bottom shadow so it merges into the panel it owns.
    p = &t.Params[ImGuiShadeClass_TabActive][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.055f;
    p->GradientBottom = -0.014f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.0f;
    p->ColBottomShadow = IM_COL32(0, 0, 0, 1);   // Deliberately near-invisible, not "unset"
    p = &t.Params[ImGuiShadeClass_TabActive][ImGuiShadeState_Hovered];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBevel | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.065f;
    p->GradientBottom = -0.010f;
    p = &t.Params[ImGuiShadeClass_TabActive][ImGuiShadeState_Disabled]; // Selected tab, unfocused bar
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.026f;
    p->GradientBottom = -0.012f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.45f;
    p->ColBottomShadow = IM_COL32(0, 0, 0, 1);

    // Slider / scrollbar grabs are little raised handles.
    p = &t.Params[ImGuiShadeClass_Grab][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.050f;
    p->GradientBottom = -0.045f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.0f;
    p = &t.Params[ImGuiShadeClass_Grab][ImGuiShadeState_Active];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_InheritBorder;
    p->GradientTop = 0.060f;
    p->GradientBottom = -0.030f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 1.3f;

    // Scrollbar track: a shallow channel for the grab to ride in.
    p = &t.Params[ImGuiShadeClass_ScrollbarBg][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_Recessed;
    p->GradientTop = -0.016f;
    p->GradientBottom = 0.008f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.5f;

    p = &t.Params[ImGuiShadeClass_TableHeader][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set;
    p->GradientTop = 0.030f;
    p->GradientBottom = -0.028f;
    p->BevelSize = 1.0f;
    p->BevelIntensity = 0.8f;

    // Separators are drawn as thin rectangles; a bevel on a 1px rect is noise, so only the
    // gradient survives here.
    p = &t.Params[ImGuiShadeClass_Separator][ImGuiShadeState_Normal];
    p->Flags = ImGuiShadeFlags_Set | ImGuiShadeFlags_NoBevel;
    p->GradientTop = -0.020f;
    p->GradientBottom = 0.030f;
}

#endif // #ifndef IMGUI_DISABLE
