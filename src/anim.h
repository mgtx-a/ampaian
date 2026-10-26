// Springs and eases: the same maths as the C# version, itself a port of the
// SwiftUI curves the macOS app uses.
#pragma once
#include "common.h"

namespace am {

enum class Curve { EaseOut, EaseIn, EaseInOut };

double BezierEase(Curve c, double x);

/// One animated number: a damped spring, or a timed ease. Retargeting a spring
/// keeps its velocity, so motions blend. Active ones are stepped together by
/// StepAll(), once per frame.
class Anim {
public:
    Anim() = default;
    Anim(const Anim&) = delete;
    Anim& operator=(const Anim&) = delete;
    ~Anim();

    double value = 0;

    void Set(double v);                                           // jump, no animation
    void Spring(double to, double response, double dampingFraction);   // SwiftUI .spring()
    void SpringRaw(double to, double stiffness, double damping);       // SwiftUI .interpolatingSpring()
    void Ease(double to, double seconds, Curve c);
    bool Active() const { return mode_ != Mode::Idle; }
    double Target() const { return target_; }

    bool Step(double dt);   // returns whether it is still moving

private:
    enum class Mode { Idle, Spring, Ease };
    void Enroll();

    Mode mode_ = Mode::Idle;
    double target_ = 0, vel_ = 0, k_ = 0, c_ = 0;
    double from_ = 0, dur_ = 1, t_ = 0;
    Curve curve_ = Curve::EaseOut;
    bool enrolled_ = false;
    friend bool StepAll(double dt, bool* anyMoved);
};

/// Advances every active animation. Returns whether any is still moving.
bool StepAll(double dt, bool* anyMoved = nullptr);
bool AnyAnimating();

}  // namespace am
