// Shelf-packed atlases. Placement depends only on the order of sizes, so it's deterministic as
// long as loaders finalize in AssetId order.
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/render/atlas.h>

namespace lam {

// ---- AtlasPacker -----------------------------------------------------------------------------

AtlasPacker::AtlasPacker(int page_size, int padding, IVec2 reserved)
    : page_size_(std::max(page_size, 1)), padding_(std::max(padding, 0)),
      reserved_{std::max(reserved.x, 0), std::max(reserved.y, 0)} {}

std::optional<AtlasPacker::Placement> AtlasPacker::add(IVec2 size) {
    if (size.x <= 0 || size.y <= 0) return std::nullopt;
    // Check before adding anything: a corrupt PNG header can claim sizes near INT_MAX.
    if (size.x > page_size_ || size.y > page_size_) return std::nullopt;
    const int p = page_size_;
    const bool has_reserved = reserved_.x > 0 && reserved_.y > 0;
    const bool fits_page = has_reserved ? ((reserved_.x + padding_ + size.x <= p && size.y <= p) ||
                                           (size.x <= p && reserved_.y + padding_ + size.y <= p))
                                        : (size.x <= p && size.y <= p);
    if (!fits_page) return std::nullopt;

    const auto place = [&](Shelf &s) {
        const Placement out{s.page, IRect{s.x, s.y, size.x, size.y}};
        s.x += size.x + padding_;
        return out;
    };

    // Best existing shelf, then a new shelf, then grow the last shelf, then a new page.
    for (int attempt = 0; attempt < 2; ++attempt) {
        int best = -1;
        int best_waste = INT_MAX;
        for (std::size_t i = 0; i < shelves_.size(); ++i) {
            const Shelf &s = shelves_[i];
            if (s.height < size.y || s.x + size.x > p) continue;
            const int waste = s.height - size.y;
            if (waste < best_waste) {
                best_waste = waste;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) return place(shelves_[static_cast<std::size_t>(best)]);
        if (pages_ > 0) {
            const int page = pages_ - 1;
            Shelf *last =
                (!shelves_.empty() && shelves_.back().page == page) ? &shelves_.back() : nullptr;
            const int next_y = last != nullptr ? last->y + last->height + padding_ : 0;
            if (size.x <= p && next_y + size.y <= p) {
                shelves_.push_back(Shelf{page, next_y, size.y, 0});
                return place(shelves_.back());
            }
            if (last != nullptr && last->x + size.x <= p && last->y + size.y <= p) {
                last->height = std::max(last->height, size.y);
                return place(*last);
            }
        }
        const int page = pages_++;
        if (has_reserved) shelves_.push_back(Shelf{page, 0, reserved_.y, reserved_.x + padding_});
    }
    return std::nullopt; // unreachable
}

int AtlasPacker::page_count() const {
    return pages_;
}

void AtlasPacker::clear() {
    shelves_.clear();
    pages_ = 0;
}

// ---- AtlasSet --------------------------------------------------------------------------------

namespace {

void blit(std::vector<std::uint8_t> &dst, int dst_w, const std::vector<std::uint8_t> &src, int w,
          int h, int x, int y) {
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4;
    for (int row = 0; row < h; ++row) {
        const std::size_t from = static_cast<std::size_t>(row) * row_bytes;
        const std::size_t to =
            (static_cast<std::size_t>(y + row) * static_cast<std::size_t>(dst_w) +
             static_cast<std::size_t>(x)) *
            4;
        std::memcpy(dst.data() + to, src.data() + from, row_bytes);
    }
}

// For page names like "@atlas/ui/0". Unnamed ids print as hex, since '#' starts a fragment.
std::string group_label(StrId group) {
    const std::string_view name = id_name(group);
    if (!name.empty()) return std::string(name);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i)
        out[static_cast<std::size_t>(15 - i)] = kHex[(group.value >> (4 * i)) & 15u];
    return out;
}

void allocate_page_pixels(Texture &page) {
    const ImageData &builtin = builtin_image();
    page.pixels.width = page.width;
    page.pixels.height = page.height;
    page.pixels.rgba.assign(
        static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4, 0);
    blit(page.pixels.rgba, page.width, builtin.rgba, builtin.width, builtin.height, 0, 0);
}

} // namespace

AtlasSet::AtlasSet(int page_size, int padding)
    : page_size_(std::max(page_size, kBuiltinImageSize * 2)), padding_(std::max(padding, 0)) {
    register_id_name(str_id("default"), "default");
}

Result<TextureRegion> AtlasSet::add(AssetServer &assets, const ImageData &image, StrId group) {
    if (image.width <= 0 || image.height <= 0) {
        return make_error(ErrorCode::Invalid, "atlas: image has no size");
    }
    if (image.has_pixels() && image.rgba.size() != static_cast<std::size_t>(image.width) *
                                                       static_cast<std::size_t>(image.height) * 4) {
        return make_error(ErrorCode::Invalid, "atlas: pixel buffer does not match the image size");
    }
    auto it =
        std::find_if(groups_.begin(), groups_.end(), [&](const Group &g) { return g.id == group; });
    if (it == groups_.end()) {
        groups_.push_back(Group{group, AtlasPacker(page_size_, padding_), {}});
        it = groups_.end() - 1;
    }
    Group &g = *it;
    const std::optional<AtlasPacker::Placement> placed = g.packer.add({image.width, image.height});
    if (!placed) {
        return make_error(ErrorCode::OutOfRange, "atlas: image " + std::to_string(image.width) +
                                                     "x" + std::to_string(image.height) +
                                                     " does not fit a " +
                                                     std::to_string(page_size_) + " page");
    }

    // Pages never shrink.
    bool created = false;
    while (g.pages.size() <= static_cast<std::size_t>(placed->page)) {
        Texture page;
        page.width = page_size_;
        page.height = page_size_;
        page.filter = TextureFilter::Point;
        page.keep_cpu = true; // later additions re-upload the page
        page.pixels.width = page_size_;
        page.pixels.height = page_size_;
        if (image.has_pixels()) allocate_page_pixels(page);
        const std::string name =
            "@atlas/" + group_label(group) + "/" + std::to_string(g.pages.size());
        g.pages.push_back(assets.add<Texture>(std::move(page), name, kRootScope));
        created = true;
    }

    const AssetId<Texture> page_id = g.pages[static_cast<std::size_t>(placed->page)];
    const IRect r = placed->rect;
    if (image.has_pixels()) {
        Texture *page = assets.get_mut<Texture>(page_id);
        if (page != nullptr) {
            if (!page->pixels.has_pixels()) allocate_page_pixels(*page);
            blit(page->pixels.rgba, page->width, image.rgba, image.width, image.height, r.x, r.y);
            if (!created) {
                ++page->version; // texture_upload re-uploads it
                assets.mark_modified(page_id);
            }
        }
    }
    TextureRegion region;
    region.texture = page_id;
    region.source = to_rect(r);
    region.size = {static_cast<float>(r.w), static_cast<float>(r.h)};
    region.trim_offset = {};
    return region;
}

std::span<const AssetId<Texture>> AtlasSet::pages(StrId group) const {
    for (const Group &g : groups_) {
        if (g.id == group) return g.pages;
    }
    return {};
}

AssetId<Texture> AtlasSet::shape_page() const {
    const std::span<const AssetId<Texture>> defaults = pages(str_id("default"));
    return defaults.empty() ? AssetId<Texture>{} : defaults.front();
}

int AtlasSet::page_size() const {
    return page_size_;
}

} // namespace lam
