#pragma once
// Load-time atlases. Image loaders pack into named groups at finalize, in AssetId order so the
// layout is deterministic. Opt out with "atlas": "none". Pages are 2048x2048 and never shrink.
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/math/rect.h>
#include <lam/render/texture.h>
namespace lam {
class AssetServer;
class AtlasPacker {                                             // shelf packer
public:
    explicit AtlasPacker(int page_size = 2048, int padding = 1, IVec2 reserved = {kBuiltinImageSize, kBuiltinImageSize});
    struct Placement { int page = 0; IRect rect; };
    std::optional<Placement> add(IVec2 size);                   // nullopt if larger than a page
    int page_count() const;
    void clear();
private:
    struct Shelf { int page, y, height, x; };
    std::vector<Shelf> shelves_;
    int page_size_, padding_, pages_ = 0;
    IVec2 reserved_;
};
class AtlasSet {                                                // App resource (RenderPlugin)
public:
    explicit AtlasSet(int page_size = 2048, int padding = 1);
    // Fails for images bigger than a page; keep those as standalone textures.
    Result<TextureRegion> add(AssetServer &assets, const ImageData &image, StrId group = str_id("default"));
    std::span<const AssetId<Texture>> pages(StrId group) const;
    AssetId<Texture> shape_page() const;                        // first "default" page, else none (slot 0)
    int page_size() const;
private:
    struct Group { StrId id; AtlasPacker packer; std::vector<AssetId<Texture>> pages; };
    std::vector<Group> groups_;
    int page_size_, padding_;
};
}
