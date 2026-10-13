// ParticlePools. Pools are carved from one slab; when the top
// is reached live pools get packed down, and only then does the slab grow, up to the budget.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <lam/core/assert.h>
#include <lam/math/scalar.h>
#include <lam/math/vec2.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>

namespace lam {
namespace {

constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxBudget = 1u << 24;
constexpr std::uint32_t kMaxPoolSlots = 65535;
constexpr std::uint32_t kMinSlabGrowth = 1024;
constexpr float kHuge = 1.0e30f;
constexpr Rect kEverything{-kHuge, -kHuge, 2 * kHuge, 2 * kHuge};

// Fractional part of a non-negative value (keeps sway phases small for det_sin).
float frac(float v) {
    return v - std::floor(v);
}

// Offset of one spawn inside `area` (relative to the emitter), sampled per `shape`. A zero-size
// area is a point and draws nothing from the RNG.
Vec2 sample_area(const Rect &area, SpawnShape shape, Rng &rng) {
    if (area.w == 0.0f && area.h == 0.0f) return {area.x, area.y};
    switch (shape) {
    case SpawnShape::Rect: {
        const float u = rng.unit();
        const float v = rng.unit();
        return {area.x + area.w * u, area.y + area.h * v};
    }
    case SpawnShape::Ellipse: {
        const float r = std::sqrt(rng.unit());
        const float a = rng.unit() * kTau;
        const Vec2 c = area.center();
        return {c.x + det_cos(a) * area.w * 0.5f * r, c.y + det_sin(a) * area.h * 0.5f * r};
    }
    case SpawnShape::Ring: {
        const float a = rng.unit() * kTau;
        const Vec2 c = area.center();
        return {c.x + det_cos(a) * area.w * 0.5f, c.y + det_sin(a) * area.h * 0.5f};
    }
    }
    return {area.x, area.y};
}

} // namespace

ParticlePools::ParticlePools(std::uint32_t max_particles, std::uint32_t max_pools)
    : max_particles_(std::min(max_particles, kMaxBudget)) {
    const std::uint32_t n = std::min(max_pools, kMaxPoolSlots);
    slots_.resize(n);
    free_.reserve(n);
    for (std::uint32_t i = n; i > 0; --i)
        free_.push_back(i - 1); // back() == slot 0: ids are handed out in slot order
    order_.reserve(n);
}

ParticlePools::Slot *ParticlePools::get(ParticlePoolId pool) {
    if (pool.index == 0 || pool.index > slots_.size()) return nullptr;
    Slot &s = slots_[pool.index - 1];
    return (s.live && s.generation == pool.generation) ? &s : nullptr;
}

const ParticlePools::Slot *ParticlePools::get(ParticlePoolId pool) const {
    if (pool.index == 0 || pool.index > slots_.size()) return nullptr;
    const Slot &s = slots_[pool.index - 1];
    return (s.live && s.generation == pool.generation) ? &s : nullptr;
}

// Packs every live pool to the front of the slab in offset order, then (if last_slot is live)
// rotates that pool's block to the end, so it can grow in place. No allocation.
void ParticlePools::compact(std::uint32_t last_slot) {
    order_.clear();
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].live && slots_[i].capacity > 0) order_.push_back(i);
    }
    // Insertion sort by offset: at most max_pools entries, and this is a rare path.
    for (std::size_t i = 1; i < order_.size(); ++i) {
        const std::uint32_t v = order_[i];
        std::size_t j = i;
        while (j > 0 && slots_[order_[j - 1]].offset > slots_[v].offset) {
            order_[j] = order_[j - 1];
            --j;
        }
        order_[j] = v;
    }
    std::uint32_t at = 0;
    for (const std::uint32_t i : order_) {
        Slot &s = slots_[i];
        if (s.offset != at && s.count > 0)
            std::memmove(slab_.data() + at, slab_.data() + s.offset, sizeof(Particle) * s.count);
        s.offset = at;
        at += s.capacity;
    }
    top_ = at;
    if (last_slot >= slots_.size() || !slots_[last_slot].live || slots_[last_slot].capacity == 0) return;
    Slot &last = slots_[last_slot];
    if (last.offset + last.capacity == top_) return;
    const std::uint32_t from = last.offset, cap = last.capacity;
    std::rotate(slab_.begin() + from, slab_.begin() + from + cap, slab_.begin() + top_);
    for (const std::uint32_t i : order_) {
        if (i != last_slot && slots_[i].offset > from) slots_[i].offset -= cap;
    }
    last.offset = top_ - cap;
}

// Gives slot `slot_index` a range of `capacity` particles. keep_particles: the slot already has
// a smaller range whose particles move along. Budget checks are the caller's.
bool ParticlePools::reserve_range(std::uint32_t slot_index, std::uint32_t capacity, bool keep_particles) {
    Slot &s = slots_[slot_index];
    const auto grow_to = [this](std::uint32_t needed) {
        if (needed <= slab_.size()) return;
        const std::size_t doubled = std::max<std::size_t>(slab_.size() * 2, kMinSlabGrowth);
        const std::size_t size = std::max<std::size_t>(needed, std::min<std::size_t>(doubled, max_particles_));
        slab_.resize(size); // counted allocation: warm-up only
    };
    const bool on_top = keep_particles && s.capacity > 0 && s.offset + s.capacity == top_;
    if (on_top) {
        if (s.offset + capacity > slab_.size()) {
            compact(slot_index);
            grow_to(s.offset + capacity);
        }
        s.capacity = capacity;
        top_ = s.offset + capacity;
        return true;
    }
    if (top_ + capacity > slab_.size()) {
        compact(keep_particles ? slot_index : kNoSlot);
        if (keep_particles) { // now the last block: extend it in place
            grow_to(s.offset + capacity);
            s.capacity = capacity;
            top_ = s.offset + capacity;
            return true;
        }
        grow_to(top_ + capacity);
    }
    if (keep_particles && s.count > 0)
        std::memcpy(slab_.data() + top_, slab_.data() + s.offset, sizeof(Particle) * s.count);
    s.offset = top_;
    s.capacity = capacity;
    top_ += capacity;
    return true;
}

ParticlePoolId ParticlePools::create(const EmitterConfig &c) {
    const EmitterConfig sc = sanitize(c);
    if (free_.empty() || reserved_ + sc.capacity > max_particles_) {
        ++rejected_;
        return {};
    }
    const std::uint32_t index = free_.back();
    free_.pop_back();
    Slot &s = slots_[index];
    s.config = sc;
    s.origin = {};
    s.rotation = 0;
    s.bounds_min = s.bounds_max = {};
    s.max_speed = 0;
    s.offset = s.capacity = s.count = s.next_replace = 0;
    s.draining = false;
    s.live = true;
    ++s.generation;
    reserve_range(index, sc.capacity, false);
    reserved_ += sc.capacity;
    return {index + 1, s.generation};
}

void ParticlePools::free_slot(std::uint32_t slot_index) {
    Slot &s = slots_[slot_index];
    reserved_ -= s.capacity;
    if (s.capacity > 0 && s.offset + s.capacity == top_) top_ = s.offset;
    s.live = false;
    s.draining = false;
    s.count = s.capacity = s.offset = s.next_replace = 0;
    // A slot whose generation is exhausted is retired, never reused.
    if (s.generation != std::numeric_limits<std::uint32_t>::max()) free_.push_back(slot_index);
}

void ParticlePools::destroy(ParticlePoolId pool) {
    if (get(pool) != nullptr) free_slot(pool.index - 1);
}

void ParticlePools::release(ParticlePoolId pool) {
    Slot *s = get(pool);
    if (s == nullptr) return;
    s->draining = true;
    if (s->count == 0) free_slot(pool.index - 1);
}

bool ParticlePools::valid(ParticlePoolId pool) const {
    return get(pool) != nullptr;
}

ParticlePools::Particle ParticlePools::make_particle(const Slot &s, Vec2 origin, Rng &rng) const {
    const EmitterConfig &c = s.config;
    Vec2 offset = sample_area(c.spawn_area, c.spawn_shape, rng);
    if (s.rotation != 0.0f) offset = rotate(offset, s.rotation);
    float angle = c.angle + s.rotation;
    if (c.spread > 0.0f) angle += rng.range(-0.5f * c.spread, 0.5f * c.spread);
    const float speed = rng.range(c.speed_min, c.speed_max);
    Particle p;
    p.position = (c.local_space ? Vec2{} : origin) + offset;
    p.velocity = Vec2{det_cos(angle), det_sin(angle)} * speed;
    p.age = 0;
    p.life = rng.range(c.life_min, c.life_max);
    p.rotation = rng.range(c.rotation_min, c.rotation_max);
    p.spin = rng.range(c.spin_min, c.spin_max);
    p.phase = c.sway > 0.0f ? rng.range(0.0f, kTau) : 0.0f;
    return p;
}

void ParticlePools::store(Slot &s, const Particle &p) {
    if (s.count == 0) {
        s.bounds_min = s.bounds_max = p.position;
    } else {
        s.bounds_min = min(s.bounds_min, p.position);
        s.bounds_max = max(s.bounds_max, p.position);
    }
    s.max_speed = std::max(s.max_speed, length(p.velocity));
    if (s.count < s.capacity) {
        slab_[s.offset + s.count++] = p;
        return;
    }
    // Full: overwrite in turn, so a constant stream keeps the newest particles (old engine).
    slab_[s.offset + s.next_replace] = p;
    s.next_replace = (s.next_replace + 1) % s.capacity;
    ++replaced_;
}

// Exact motion under constant gravity g and drag d (dv/dt = g - d v) over dt seconds.
void ParticlePools::advance(Particle &p, const Slot &s, float dt) {
    const EmitterConfig &c = s.config;
    p.age += dt;
    if (c.drag > 0.0f) {
        const float x = c.drag * dt;
        const float k = std::exp(-x);
        const float gone = -std::expm1(-x) / c.drag;
        const Vec2 terminal = c.gravity / c.drag;
        p.position += terminal * dt + (p.velocity - terminal) * gone;
        p.velocity = terminal + (p.velocity - terminal) * k;
    } else {
        p.position += p.velocity * dt + c.gravity * (0.5f * dt * dt);
        p.velocity += c.gravity * dt;
    }
    p.rotation += p.spin * dt;
}

void ParticlePools::emit(ParticlePoolId pool, Vec2 origin, std::uint16_t count, Rng &rng) {
    Slot *s = get(pool);
    if (s == nullptr || s->draining || count == 0) return;
    if (s->config.local_space) s->origin = origin;
    // More than `capacity` would only overwrite each other: emit the last `capacity`.
    const std::uint32_t n = std::min<std::uint32_t>(count, s->capacity);
    for (std::uint32_t i = 0; i < n; ++i)
        store(*s, make_particle(*s, origin, rng));
}

void ParticlePools::emit_stream(ParticlePoolId pool, Vec2 from, Vec2 to, std::uint32_t count, float seconds,
                                Rng &rng) {
    Slot *s = get(pool);
    if (s == nullptr || s->draining || count == 0) return;
    if (!(seconds > 0.0f) || !std::isfinite(seconds)) seconds = 0.0f;
    if (s->config.local_space) s->origin = to;
    // Only the newest `capacity` births can survive the overwrite order: skip the older ones.
    const std::uint32_t first = count > s->capacity ? count - s->capacity : 0;
    const float inv = 1.0f / static_cast<float>(count);
    for (std::uint32_t i = first; i < count; ++i) {
        const float born = (static_cast<float>(i) + 0.5f) * inv; // 0 = start of the interval
        const float age = seconds * (1.0f - born);
        Particle p = make_particle(*s, lerp(from, to, born), rng);
        if (age >= p.life) continue; // born and already dead within this interval
        if (age > 0.0f) advance(p, *s, age);
        store(*s, p);
    }
}

void ParticlePools::set_origin(ParticlePoolId pool, Vec2 origin, float rotation) {
    Slot *s = get(pool);
    if (s == nullptr) return;
    s->origin = is_finite(origin) ? origin : Vec2{};
    s->rotation = std::isfinite(rotation) ? rotation : 0.0f;
}

bool ParticlePools::set_config(ParticlePoolId pool, const EmitterConfig &c) {
    Slot *s = get(pool);
    if (s == nullptr) return false;
    const EmitterConfig sc = sanitize(c);
    const std::uint32_t index = pool.index - 1;
    if (sc.capacity < s->capacity) {
        reserved_ -= s->capacity - sc.capacity;
        if (s->offset + s->capacity == top_) top_ = s->offset + sc.capacity;
        s->capacity = sc.capacity;
        s->count = std::min(s->count, sc.capacity);
        s->next_replace = 0;
    } else if (sc.capacity > s->capacity) {
        if (reserved_ - s->capacity + sc.capacity > max_particles_) return false;
        reserved_ += sc.capacity - s->capacity;
        reserve_range(index, sc.capacity, true);
    }
    // Switching space keeps every particle where it is on screen.
    if (sc.local_space != s->config.local_space) {
        const Vec2 shift = sc.local_space ? -s->origin : s->origin;
        Particle *base = slab_.data() + s->offset;
        for (std::uint32_t i = 0; i < s->count; ++i)
            base[i].position += shift;
        s->bounds_min += shift;
        s->bounds_max += shift;
    }
    s->config = sc;
    return true;
}

const EmitterConfig *ParticlePools::config(ParticlePoolId pool) const {
    const Slot *s = get(pool);
    return s != nullptr ? &s->config : nullptr;
}

void ParticlePools::clear(ParticlePoolId pool) {
    Slot *s = get(pool);
    if (s == nullptr) return;
    s->count = 0;
    s->next_replace = 0;
}

void ParticlePools::clear() {
    for (Slot &s : slots_) {
        s.count = 0;
        s.next_replace = 0;
    }
}

void ParticlePools::update(float dt, Rng &rng) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        Slot &s = slots_[index];
        if (!s.live) continue;
        if (s.count == 0) {
            if (s.draining) free_slot(index);
            continue;
        }
        const EmitterConfig &c = s.config;
        // Per-pool constants of the exact integrator (see advance()).
        const bool drag = c.drag > 0.0f;
        const float k = drag ? std::exp(-c.drag * dt) : 1.0f;
        const float gone = drag ? -std::expm1(-c.drag * dt) / c.drag : dt;
        const Vec2 terminal = drag ? c.gravity / c.drag : Vec2{};
        const Vec2 fall = c.gravity * (0.5f * dt * dt);
        const Vec2 dv = c.gravity * dt;
        // Uniform noise in [-j, j] has variance j^2 / 3 = jitter^2 * dt: Brownian, so the spread
        // after a second does not depend on the frame rate.
        const float j = c.jitter > 0.0f ? c.jitter * std::sqrt(3.0f * dt) : 0.0f;
        Particle *base = slab_.data() + s.offset;
        std::uint32_t n = s.count;
        Vec2 lo{kHuge, kHuge}, hi{-kHuge, -kHuge};
        float speed_sq = 0;
        std::uint32_t i = 0;
        while (i < n) {
            Particle &p = base[i];
            p.age += dt;
            if (p.age >= p.life) {
                p = base[--n]; // swap-remove; the moved particle is processed next
                continue;
            }
            if (drag) {
                p.position += terminal * dt + (p.velocity - terminal) * gone;
                p.velocity = terminal + (p.velocity - terminal) * k;
            } else {
                p.position += p.velocity * dt + fall;
                p.velocity += dv;
            }
            if (j > 0.0f) {
                p.velocity.x += rng.range(-j, j);
                p.velocity.y += rng.range(-j, j);
            }
            p.rotation += p.spin * dt;
            lo = min(lo, p.position);
            hi = max(hi, p.position);
            speed_sq = std::max(speed_sq, length_sq(p.velocity));
            ++i;
        }
        s.count = n;
        if (s.next_replace >= n) s.next_replace = 0;
        s.bounds_min = lo;
        s.bounds_max = hi;
        s.max_speed = std::sqrt(speed_sq);
        if (n == 0 && s.draining) free_slot(index);
    }
}

void ParticlePools::draw(RenderQueue &q, const Rect &visible_world) const {
    draw(q, visible_world, kEverything);
}

void ParticlePools::draw(RenderQueue &q, const Rect &visible_world, const Rect &visible_screen) const {
    drawn_ = 0;
    culled_ = 0;
    for (const Slot &s : slots_) {
        if (!s.live || s.count == 0) continue;
        draw_slot(q, s, is_screen_layer(s.config.layer) ? visible_screen : visible_world);
    }
}

void ParticlePools::draw_slot(RenderQueue &q, const Slot &s, const Rect &view) const {
    const EmitterConfig &c = s.config;
    const Vec2 base = c.local_space ? s.origin : Vec2{};
    // Coarse test of the whole pool: positions' bounds grown by the largest possible quad.
    const float max_size = std::max(c.size_start, c.size_end);
    const float margin =
        max_size * 0.7072f + c.sway + c.stretch * s.max_speed * 0.5f + 1.0f; // half diagonal
    const Rect bounds = Rect::from_min_max(s.bounds_min + base - Vec2{margin, margin},
                                           s.bounds_max + base + Vec2{margin, margin});
    if (!bounds.overlaps(view)) {
        culled_ += s.count;
        return;
    }
    // Texture: a region's own, or the shape page's built-in block (circle unless a source is set).
    const bool own = static_cast<bool>(c.region.texture);
    const AssetId<Texture> texture = own ? c.region.texture : q.shape_texture();
    const Rect src = (!own && c.region.source.empty()) ? kCircleSource : c.region.source;
    const float fw = c.region.size.x > 0.0f ? c.region.size.x : src.w;
    const float fh = c.region.size.y > 0.0f ? c.region.size.y : src.h;
    const float longest = std::max(std::max(fw, fh), 1e-6f);
    const float ux = 0.5f * fw / longest, uy = 0.5f * fh / longest; // half extents per pixel of size
    const float diag = std::sqrt(ux * ux + uy * uy);                // half diagonal per pixel of size
    const float u0 = src.x, v0 = src.y, u1 = src.x + src.w, v1 = src.y + src.h;
    const bool aligned = c.align_to_velocity || c.stretch > 0.0f;
    const float view_x1 = view.x + view.w, view_y1 = view.y + view.h;
    const Particle *particles = slab_.data() + s.offset;
    for (std::uint32_t i = 0; i < s.count; ++i) {
        const Particle &p = particles[i];
        const float t = std::min(p.age / p.life, 1.0f);
        const float size = c.size_start + (c.size_end - c.size_start) * t;
        if (!(size > 0.0f)) continue;
        Vec2 pos = base + p.position;
        // Cull before any colour or trig work: exact extents for axis-aligned quads, the half
        // diagonal (plus the streak) for turned ones; sway can move the quad sideways.
        const bool rotated = aligned || p.rotation != 0.0f;
        const float speed = aligned ? length(p.velocity) : 0.0f;
        float hx = size * ux;
        const float hy = size * uy;
        if (aligned) hx += speed * c.stretch * 0.5f;
        const float ex = (rotated ? size * diag + speed * c.stretch * 0.5f : hx) + c.sway;
        const float ey = rotated ? size * diag + speed * c.stretch * 0.5f : hy;
        if (pos.x + ex <= view.x || pos.x - ex >= view_x1 || pos.y + ey <= view.y || pos.y - ey >= view_y1) {
            ++culled_;
            continue;
        }
        const Color color = lerp(c.color_start, c.color_end, t);
        if (color.a == 0) continue;
        if (c.sway > 0.0f) pos.x += c.sway * det_sin(p.phase + kTau * frac(p.age * c.sway_hz));
        Vec2 ax{1, 0};
        if (aligned) {
            ax = speed > 1e-4f ? p.velocity / speed : Vec2{det_cos(p.rotation), det_sin(p.rotation)};
        } else if (rotated) {
            ax = {det_cos(p.rotation), det_sin(p.rotation)};
        }
        const Vec2 dx = ax * hx;
        const Vec2 dy = rotated ? perp(ax) * hy : Vec2{0, hy};
        const QuadVertex v[4] = {{pos - dx - dy, {u0, v0}, color},
                                 {pos + dx - dy, {u1, v0}, color},
                                 {pos + dx + dy, {u1, v1}, color},
                                 {pos - dx + dy, {u0, v1}, color}};
        q.push(c.layer, c.y_sort ? pos.y + c.depth : c.depth, c.material, texture, v);
        ++drawn_;
    }
}

std::span<const ParticlePools::Particle> ParticlePools::particles(ParticlePoolId pool) const {
    const Slot *s = get(pool);
    if (s == nullptr || s->count == 0) return {};
    return {slab_.data() + s->offset, s->count};
}

std::uint32_t ParticlePools::alive(ParticlePoolId pool) const {
    const Slot *s = get(pool);
    return s != nullptr ? s->count : 0;
}

std::uint32_t ParticlePools::total_alive() const {
    std::uint32_t n = 0;
    for (const Slot &s : slots_) {
        if (s.live) n += s.count;
    }
    return n;
}

std::uint32_t ParticlePools::capacity(ParticlePoolId pool) const {
    const Slot *s = get(pool);
    return s != nullptr ? s->capacity : 0;
}

ParticlePoolStats ParticlePools::stats() const {
    ParticlePoolStats st;
    for (const Slot &s : slots_) {
        if (!s.live) continue;
        ++st.pools;
        st.alive += s.count;
    }
    st.reserved = reserved_;
    st.rejected = rejected_;
    st.replaced = replaced_;
    st.drawn = drawn_;
    st.culled = culled_;
    st.slab_size = static_cast<std::uint32_t>(slab_.size());
    return st;
}

} // namespace lam
