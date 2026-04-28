#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace pf {

using Fix = int32_t;
constexpr Fix kScale = 1000;

constexpr Fix fx(int whole) { return whole * kScale; }
Fix fxFromFloat(float value);
float fxToFloat(Fix value);
Fix fxMul(Fix a, Fix b);
Fix fxDiv(Fix a, Fix b);
Fix fxApproach(Fix value, Fix target, Fix delta);
Fix fxAbs(Fix value);

struct Vec2 {
    Fix x = 0;
    Fix y = 0;
};

struct Vec3 {
    Fix x = 0;
    Fix y = 0;
    Fix z = 0;
};

Vec2 operator+(Vec2 a, Vec2 b);
Vec2 operator-(Vec2 a, Vec2 b);
Vec2 operator*(Vec2 a, Fix scalar);
Vec2& operator+=(Vec2& a, Vec2 b);
Vec2& operator-=(Vec2& a, Vec2 b);

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, Fix scalar);
Vec3& operator+=(Vec3& a, Vec3 b);

Fix dot(Vec2 a, Vec2 b);
Fix dot(Vec3 a, Vec3 b);
Fix lengthSquared(Vec2 v);
Fix distanceSquared(Vec3 a, Vec3 b);
Vec2 clampMagnitude(Vec2 v, Fix maxLength);

struct Capsule {
    Vec3 a;
    Vec3 b;
    Fix radius = fx(1);
};

bool sphereSphere(Vec3 a, Fix ar, Vec3 b, Fix br);
bool capsuleCapsule(Capsule a, Capsule b);
std::string toString(Vec2 v);

} // namespace pf

