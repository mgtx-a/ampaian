// The strip along the top of the screen: the rope and everything hanging from it.
#pragma once
#include "anim.h"
#include "common.h"
#include "gfx.h"
#include "layout.h"
#include "model.h"

namespace am {

/// One photo's card: what it is doing right now.
struct Card {
    uint32_t id = 0;
    Pegged* item = nullptr;
    Anim X, swing, arrived, visible, hover, scale, copiedT;
    bool hovering = false, pressed = false, dragging = false, copied = false, flying = false, falling = false;
    double cardW = 0, cardH = 0;
};

class Panel {
public:
    explicit Panel(Line& line);
    ~Panel();

    bool Create(HINSTANCE inst);
    HWND hwnd() const { return hwnd_; }

    void PlaceOnScreen(const Mon& mon);
    const Mon& screen() const { return mon_; }
    RECT pixelFrame() const { return frame_; }       // the window, in screen pixels
    double widthDip() const { return widthDip_; }

    void Show(bool on);
    bool shown() const { return shown_; }
    void SetClickThrough(bool on);
    void SetRevealed(bool on);
    bool slidFullyUp() const { return slide_.value <= -(L::PanelHeight + 11); }

    /// Lays the cards out along the rope; new ones appear, gone ones leave, the rest spring.
    void Sync(bool snap = false);
    void MarkDirty() { dirty_ = true; }
    bool dirty() const { return dirty_ && shown_; }
    /// Draws and presents when something changed or moves.
    void Draw(bool animating = false);
    void UpdateHover();

    /// Where card `id` hangs at rest, in screen pixels (unrotated), for the flight.
    bool CardFrame(uint32_t id, RectD* out) const;

    Card* FindCard(uint32_t id);
    void OnLineState();                 // copied / pressed / dragging changed
    void Breeze();                      // a gust of wind
    void Nudge(Card& c, double degrees);

    // Used by drag and drop.
    std::function<void(Card&, POINT startPx)> beginDrag;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    friend struct PanelAccess;
    void DrawRope(double width);
    void DrawHint(double width);
    void DrawCard(Card& c, double width);
    void RefreshCard(Card& c);
    Card* HitCard(double dipX, double dipY, double* localX, double* localY);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnMouseDown(int px, int py, bool dbl);
    void OnMouseMove(int px, int py);
    void OnMouseUp(int px, int py);
    void OnRightClick(int px, int py);
    void SetPressed(Card* c, bool on);
    void EndPress();
    void ContextMenu(Card& c, POINT screenPx);
    void Hover(Card* c);

    Line& line_;
    ID2D1DCRenderTarget* rt_ = nullptr;         // only while drawing
    D2D1::Matrix3x2F stage_ = D2D1::Matrix3x2F::Identity();
    Com<ID2D1PathGeometry> ropeGeo_;
    double ropeW_ = -1;
    HWND hwnd_ = nullptr;
    Surface surf_;
    Mon mon_{};
    RECT frame_{};
    double widthDip_ = 0;
    bool shown_ = false, dirty_ = true, clickThrough_ = true;
    Anim slide_, hintOpacity_;
    std::vector<std::unique_ptr<Card>> cards_;

    // input
    Card* downCard_ = nullptr;
    POINT downPx_{};
    bool startedDrag_ = false, didLongPress_ = false, ignoreNextUp_ = false;
    uint32_t holdToken_ = 0;
};

}  // namespace am
