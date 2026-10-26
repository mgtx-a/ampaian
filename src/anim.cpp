#include "anim.h"

namespace am {

static std::vector<Anim*>& Registry()
{
    static std::vector<Anim*> list;
    return list;
}

static double CubicAt(double a, double b, double t)
{
    double u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static double CubicSlope(double a, double b, double t)
{
    double u = 1 - t;
    return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}

static double SolveBezier(double x1, double y1, double x2, double y2, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double t = x;
    for (int i = 0; i < 10; i++) {
        double err = CubicAt(x1, x2, t) - x;
        double d = CubicSlope(x1, x2, t);
        if (std::fabs(err) < 1e-6 || std::fabs(d) < 1e-6) break;
        t -= err / d;
    }
    return CubicAt(y1, y2, std::clamp(t, 0.0, 1.0));
}

double BezierEase(Curve c, double x)
{
    switch (c) {
        case Curve::EaseOut: return SolveBezier(0, 0, 0.58, 1, x);
        case Curve::EaseIn: return SolveBezier(0.42, 0, 1, 1, x);
        default: return SolveBezier(0.42, 0, 0.58, 1, x);
    }
}

Anim::~Anim()
{
    if (enrolled_) {
        auto& r = Registry();
        r.erase(std::remove(r.begin(), r.end(), this), r.end());
    }
}

void Anim::Enroll()
{
    if (enrolled_) return;
    enrolled_ = true;
    Registry().push_back(this);
}

void Anim::Set(double v)
{
    mode_ = Mode::Idle;
    value = target_ = v;
    vel_ = 0;
}

void Anim::Spring(double to, double response, double dampingFraction)
{
    double w = 2 * 3.14159265358979323846 / response;
    SpringRaw(to, w * w, 2 * dampingFraction * w);
}

void Anim::SpringRaw(double to, double k, double c)
{
    target_ = to; k_ = k; c_ = c;
    mode_ = Mode::Spring;
    Enroll();
}

void Anim::Ease(double to, double seconds, Curve c)
{
    from_ = value; target_ = to; dur_ = seconds; t_ = 0; curve_ = c; vel_ = 0;
    mode_ = Mode::Ease;
    Enroll();
}

bool Anim::Step(double dt)
{
    if (mode_ == Mode::Spring) {
        int n = std::max(1, int(std::ceil(dt * 240)));
        double h = dt / n;
        for (int i = 0; i < n; i++) {
            double a = -k_ * (value - target_) - c_ * vel_;
            vel_ += a * h;
            value += vel_ * h;
            if (std::fabs(value - target_) < 5e-4 && std::fabs(vel_) < 5e-3) {
                value = target_; vel_ = 0; mode_ = Mode::Idle;
                break;
            }
        }
    } else if (mode_ == Mode::Ease) {
        t_ += dt;
        double p = std::min(1.0, t_ / dur_);
        value = from_ + (target_ - from_) * BezierEase(curve_, p);
        if (p >= 1) { value = target_; mode_ = Mode::Idle; }
    }
    return mode_ != Mode::Idle;
}

bool StepAll(double dt, bool* anyMoved)
{
    auto& r = Registry();
    bool moved = !r.empty();
    // Iterate over a copy: stepping can end an animation and drop it from the list.
    std::vector<Anim*> snapshot = r;
    for (Anim* a : snapshot) {
        if (!a->Step(dt)) {
            a->enrolled_ = false;
            r.erase(std::remove(r.begin(), r.end(), a), r.end());
        }
    }
    if (anyMoved) *anyMoved = moved;
    return !r.empty();
}

bool AnyAnimating() { return !Registry().empty(); }

}  // namespace am
