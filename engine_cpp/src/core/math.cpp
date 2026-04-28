#include "core/math.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pf {

Fix fxFromFloat(float value) {
    return static_cast<Fix>(std::lround(value * static_cast<float>(kScale)));
}

float fxToFloat(Fix value) {
    return static_cast<float>(value) / static_cast<float>(kScale);
}

Fix fxMul(Fix a, Fix b) {
    return static_cast<Fix>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) / kScale);
}

Fix fxDiv(Fix a, Fix b) {
    if (b == 0) {
        return 0;
    }
    return static_cast<Fix>((static_cast<int64_t>(a) * kScale) / b);
}

Fix fxApproach(Fix value, Fix target, Fix delta) {
    if (value < target) {
        return std::min<Fix>(value + delta, target);
    }
    if (value > target) {
        return std::max<Fix>(value - delta, target);
    }
    return value;
}

Fix fxAbs(Fix value) {
    return value < 0 ? -value : value;
}

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(Vec2 a, Fix scalar) { return {fxMul(a.x, scalar), fxMul(a.y, scalar)}; }
Vec2& operator+=(Vec2& a, Vec2 b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}
Vec2& operator-=(Vec2& a, Vec2 b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, Fix scalar) { return {fxMul(a.x, scalar), fxMul(a.y, scalar), fxMul(a.z, scalar)}; }
Vec3& operator+=(Vec3& a, Vec3 b) {
    a.x += b.x;
    a.y += b.y;
    a.z += b.z;
    return a;
}

Fix dot(Vec2 a, Vec2 b) {
    return fxMul(a.x, b.x) + fxMul(a.y, b.y);
}

Fix dot(Vec3 a, Vec3 b) {
    return fxMul(a.x, b.x) + fxMul(a.y, b.y) + fxMul(a.z, b.z);
}

Fix lengthSquared(Vec2 v) {
    return dot(v, v);
}

Fix distanceSquared(Vec3 a, Vec3 b) {
    return dot(a - b, a - b);
}

Vec2 clampMagnitude(Vec2 v, Fix maxLength) {
    const float x = fxToFloat(v.x);
    const float y = fxToFloat(v.y);
    const float len = std::sqrt(x * x + y * y);
    const float maxLen = fxToFloat(maxLength);
    if (len <= maxLen || len <= 0.0001f) {
        return v;
    }
    const float scale = maxLen / len;
    return {fxFromFloat(x * scale), fxFromFloat(y * scale)};
}

bool sphereSphere(Vec3 a, Fix ar, Vec3 b, Fix br) {
    const Fix r = ar + br;
    return distanceSquared(a, b) <= fxMul(r, r);
}

static float distPointSegmentSq(Vec3 p, Vec3 a, Vec3 b) {
    const float px = fxToFloat(p.x);
    const float py = fxToFloat(p.y);
    const float pz = fxToFloat(p.z);
    const float ax = fxToFloat(a.x);
    const float ay = fxToFloat(a.y);
    const float az = fxToFloat(a.z);
    const float bx = fxToFloat(b.x);
    const float by = fxToFloat(b.y);
    const float bz = fxToFloat(b.z);

    const float abx = bx - ax;
    const float aby = by - ay;
    const float abz = bz - az;
    const float apx = px - ax;
    const float apy = py - ay;
    const float apz = pz - az;
    const float denom = abx * abx + aby * aby + abz * abz;
    const float t = denom <= 0.00001f ? 0.0f : std::clamp((apx * abx + apy * aby + apz * abz) / denom, 0.0f, 1.0f);
    const float cx = ax + abx * t;
    const float cy = ay + aby * t;
    const float cz = az + abz * t;
    const float dx = px - cx;
    const float dy = py - cy;
    const float dz = pz - cz;
    return dx * dx + dy * dy + dz * dz;
}

bool capsuleCapsule(Capsule a, Capsule b) {
    // This deliberately favors stable game behavior over geometric perfection for the first slice.
    // A tighter segment-segment distance can replace it without changing the data model.
    const float r = fxToFloat(a.radius + b.radius);
    const float d0 = distPointSegmentSq(a.a, b.a, b.b);
    const float d1 = distPointSegmentSq(a.b, b.a, b.b);
    const float d2 = distPointSegmentSq(b.a, a.a, a.b);
    const float d3 = distPointSegmentSq(b.b, a.a, a.b);
    return std::min(std::min(d0, d1), std::min(d2, d3)) <= r * r;
}

std::string toString(Vec2 v) {
    std::ostringstream out;
    out << "(" << fxToFloat(v.x) << ", " << fxToFloat(v.y) << ")";
    return out.str();
}

} // namespace pf

