#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>
#include <utility>

namespace rae {

// Synchronous, optional startup feedback. Observers must not call back into the
// loader or keep the borrowed text. No dependency on the editor or graphics API.
class LoadProgress {
public:
    using Callback = std::function<void(float, std::string_view, std::string_view)>;

    LoadProgress() = default;
    explicit LoadProgress(Callback callback) : callback_(std::move(callback)) {}
    explicit operator bool() const noexcept { return static_cast<bool>(callback_); }

    void report(float fraction, std::string_view stage, std::string_view detail = {}) const {
        if (callback_) callback_(normalized(fraction), stage, detail);
    }

    // A child loader owns [0,1]; its caller assigns the enclosing startup range.
    [[nodiscard]] LoadProgress range(float begin, float end) const {
        if (!callback_) return {};
        begin = normalized(begin);
        end = std::max(begin, normalized(end));
        return LoadProgress([parent = *this, begin, end](float fraction,
            std::string_view stage, std::string_view detail) {
            parent.report(begin + (end - begin) * fraction, stage, detail);
        });
    }

private:
    static float normalized(float value) noexcept {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }
    Callback callback_;
};

} // namespace rae
