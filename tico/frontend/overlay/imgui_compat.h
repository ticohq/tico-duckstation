// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The overlay is written for ImGui 1.92 (ImTextureID an integer, images drawn
// through ImTextureRef); DuckStation's ImGui is 1.90, where ImTextureID is a
// pointer (a GPUTexture*) and images take the ImTextureID itself.

#include "imgui.h"

#include <cstdint>
#include <vector>

namespace SwitchFrontend {

// A texture id stored as an integer (the overlay's unsigned long long) as
// ImGui's ImTextureID, whichever that is.
template <typename T>
inline ImTextureID ToTextureId(T id) {
    return (ImTextureID)(std::uintptr_t)id;
}

inline unsigned long long FromTextureId(ImTextureID id) {
    return (unsigned long long)(std::uintptr_t)id;
}

#if IMGUI_VERSION_NUM < 19200
inline ImTextureID ImTextureRef(ImTextureID id) {
    return id;
}
#endif

// Fills the current path, which may be concave (ImDrawList::PathFillConcave
// arrived in 1.91), by ear clipping, and clears it.
inline void PathFillConcave(ImDrawList* dl, ImU32 color) {
#if IMGUI_VERSION_NUM >= 19100
    dl->PathFillConcave(color);
#else
    std::vector<ImVec2> points(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
    dl->PathClear();
    if (points.size() < 3) {
        return;
    }
    const auto cross = [](ImVec2 a, ImVec2 b, ImVec2 c) {
        return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
    };
    float area = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const ImVec2 a = points[i];
        const ImVec2 b = points[(i + 1) % points.size()];
        area += (a.x * b.y) - (b.x * a.y);
    }
    const float winding = area >= 0.0f ? 1.0f : -1.0f;
    std::vector<std::size_t> left;
    for (std::size_t i = 0; i < points.size(); ++i) {
        left.push_back(i);
    }
    std::size_t guard = left.size() * left.size();
    std::size_t i = 0;
    while (left.size() > 3 && guard-- > 0) {
        const std::size_t n = left.size();
        const ImVec2 a = points[left[(i + n - 1) % n]];
        const ImVec2 b = points[left[i % n]];
        const ImVec2 c = points[left[(i + 1) % n]];
        bool ear = cross(a, b, c) * winding > 0.0f;
        for (std::size_t j = 0; ear && j < n; ++j) {
            const ImVec2 p = points[left[j]];
            if (j == (i + n - 1) % n || j == i % n || j == (i + 1) % n) {
                continue;
            }
            if (cross(a, b, p) * winding >= 0.0f && cross(b, c, p) * winding >= 0.0f &&
                cross(c, a, p) * winding >= 0.0f) {
                ear = false;
            }
        }
        if (ear) {
            dl->AddTriangleFilled(a, b, c, color);
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(i % n));
        } else {
            ++i;
        }
        i %= left.size();
    }
    if (left.size() == 3) {
        dl->AddTriangleFilled(points[left[0]], points[left[1]], points[left[2]], color);
    }
#endif
}

} // namespace SwitchFrontend
