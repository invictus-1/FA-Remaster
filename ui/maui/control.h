// The control tree: the engine side of the game's interface scripts (/lua/maui/*.lua).
//
// A control is made by a script: the Lua class instance calls InternalCreate<Kind>(self, parent),
// the engine makes its object, ties it to that table (`_c_object`), puts the layout LazyVars into
// the table (Left, Right, Top, Bottom, Width, Height, Depth) and calls self:OnInit(). From then on
// scripts drive it through the methods of moho.control_methods and of its own class table, and the
// engine calls back: OnFrame, OnHide, OnDestroy, HandleEvent, ...
//
// What the original does is noted with the address where it was read in the original executable
// ("FA exe 0x..."); what only follows from the scripts' use is marked "inferred".
#pragma once
#include <string>
#include <vector>

#include "draw/draw.h"
#include "maui/lazyvar.h"
#include "script/lua.hpp"
#include "sim/script_object.h"

namespace moho::ui {

class DrawTarget;
class Frame;
class UiManager;
struct UiEvent;

class Control : public ScriptObject {
 public:
  Control() = default;
  ~Control() override = default;

  // The original's constructor (0x7867b0), in its order: the seven LazyVars are made, the object
  // is tied to the Lua table, a control with a parent joins the parent's children, takes the
  // parent's hidden state through SetHidden (so OnHide runs once, before the LazyVars are in the
  // table) and the parent's root frame; then the LazyVars go into the table.
  void Create(lua_State* L, int luaObj, Control* parent);
  // self:OnInit(), called by the Internal* creation functions after the constructor.
  void RunInit(lua_State* L) { RunScript(L, "OnInit"); }

  virtual const char* ClassName() const { return "control"; }
  // One line of this control's own state for Dump (texture, text, ...).
  virtual void Describe(std::string& out) const { (void)out; }
  // Draw this control alone (its children draw themselves). A plain control draws nothing.
  virtual void Draw(lua_State* L, DrawTarget& target) {
    (void)L;
    (void)target;
  }
  // The rectangle the LazyVars give right now.
  void GetRect(float* x0, float* y0, float* x1, float* y1) const {
    *x0 = left.Get(), *y0 = top.Get(), *x1 = right.Get(), *y1 = bottom.Get();
  }

  // Destroy (0x786ef0): leave the parent, join the root frame's list of dead controls (they are
  // deleted at the end of the frame), run self:OnDestroy(), then destroy the children, newest first.
  virtual void Destroy(lua_State* L);
  void ClearChildren(lua_State* L);
  // SetHidden (0x787170): self:OnHide(hidden) runs first, every time. If it returns true nothing
  // else happens (the control keeps its state); otherwise the control and then every child,
  // oldest first, takes the new state.
  virtual void SetHidden(lua_State* L, bool hidden);
  bool IsHidden() const { return hidden_; }
  // DisableHitTest / EnableHitTest (0x787210): the flag, and with `recursive` the children's too.
  void SetHitTestDisabled(bool disabled, bool recursive);
  bool IsHitTestDisabled() const { return hitTestDisabled_; }
  // HitTest (0x787270): Left <= x < Right and Top <= y < Bottom.
  virtual bool HitTest(float x, float y) const;
  void SetParent(Control* parent);
  void SetAlpha(float alpha, bool children);

  // What a scrollbar asks of the control it scrolls (FA exe: the control's virtual slots 18-22).
  // A control hands these to its script (IsScrollable, GetScrollValues, ScrollLines, ScrollPages,
  // ScrollSetTop); an item list answers them itself. Axis 0 is vertical, 1 horizontal ("Vert" and
  // "Horz" for scripts). GetScrollValues fills: range min, range max, visible min, visible max.
  virtual bool IsScrollable(lua_State* L, int axis);
  virtual void GetScrollValues(lua_State* L, int axis, float out[4]);
  virtual void ScrollLines(lua_State* L, int axis, float lines);
  virtual void ScrollPages(lua_State* L, int axis, float pages);
  virtual void ScrollSetTop(lua_State* L, int axis, float top);

  // An event for this control (FA exe: virtual slot 12, 0x7873a0): the script's
  // self:HandleEvent(event); true when it handled the event. Edit fields and item lists do their
  // own work here instead.
  virtual bool HandleEvent(lua_State* L, const UiEvent& e);
  // One frame of time for a control that asked for frame updates (slot 13, 0x787420):
  // self:OnFrame(seconds).
  virtual void FrameUpdate(lua_State* L, float seconds);
  // Give up the keyboard focus if this control has it (slot 15, 0x787480).
  virtual void AbandonKeyboardFocus(lua_State* L);
  // A mouse button went down on another control while this one had the keyboard focus (slot 16,
  // 0x787440): self:OnLoseKeyboardFocus(). The focus itself stays unless the script gives it up.
  virtual void LosingKeyboardFocus(lua_State* L) { RunScript(L, "OnLoseKeyboardFocus"); }
  // For a control that posted itself as the dragger (UiManager::PostEngineDragger): the mouse
  // moved, and the button was let go.
  virtual void DragMove(lua_State* L, const UiEvent& e) { (void)L, (void)e; }
  virtual void DragRelease(lua_State* L, const UiEvent& e) { (void)L, (void)e; }

  // self:<name>(args): the `nargs` values on top of L are the arguments. Returns true when the
  // function exists and ran without error; then `nresults` results are on the stack. A script
  // error is logged.
  bool RunScript(lua_State* L, const char* name, int nargs = 0, int nresults = 0);

  Control* parent() const { return parent_; }
  Frame* root() const { return root_; }
  const std::vector<Control*>& children() const { return children_; }  // oldest first
  bool dead() const { return dead_; }

  LazyVar left, right, top, bottom, width, height, depth;
  std::string name;
  float alpha = 1.0f;
  int renderPass = 0;
  bool needsFrameUpdate = false;

 protected:
  friend class Frame;
  friend class UiManager;
  // An extra LazyVar of a control class (BitmapWidth, TextAdvance, ...): made and put in the table.
  void AddLazyVar(lua_State* L, LazyVar& var, const char* field);

  Control* parent_ = nullptr;
  Frame* root_ = nullptr;
  std::vector<Control*> children_;
  bool hidden_ = false;
  bool hitTestDisabled_ = false;
  bool dead_ = false;
};

// A group: a control with nothing of its own (moho.group_methods).
class Group : public Control {
 public:
  const char* ClassName() const override { return "group"; }
};

// A control class whose own behaviour is not built yet: it lays out like any control, and its own
// methods are still logged stubs.
class PlainControl : public Control {
 public:
  explicit PlainControl(const char* className) : className_(className) {}
  const char* ClassName() const override { return className_; }

 private:
  const char* className_;
};

// A root frame: one per head (screen). It covers the head and owns the controls on it.
class Frame : public Control {
 public:
  const char* ClassName() const override { return "frame"; }
  void Destroy(lua_State* L) override;  // a root frame is not destroyed; see ClearFrame
  void MakeRoot() { root_ = this; }

  // One frame of time (0x7965a0): every living control below the frame that asked for frame
  // updates gets self:OnFrame(seconds), hidden or not, in depth-first order; then the controls
  // destroyed since the last frame are deleted.
  void Update(lua_State* L, float seconds);
  void PurgeDeleted();
  // The largest Depth of the controls in the frame (GetTopmostDepth).
  float TopmostDepth() const;

  int head = 0;
  int targetHead = 0;

 private:
  friend class Control;
  std::vector<Control*> deadList_;
};

// The interface as a whole: the root frames, the cursor, keyboard focus and input capture.
class UiManager {
 public:
  ~UiManager();

  // Make the root frame of each head the way the original does (0x7961b0): an instance of the
  // script class Frame from /lua/maui/frame.lua, then Left = Top = 0, Width and Height = the
  // head's size in pixels.
  bool CreateRootFrames(lua_State* L, int width, int height);
  void Resize(int width, int height);
  Frame* GetFrame(int head) const;
  int NumFrames() const { return static_cast<int>(frames_.size()); }
  bool initialized() const { return !frames_.empty(); }

  void Update(lua_State* L, float seconds);
  // Draw a head's controls onto a target, in the original's order.
  void Render(lua_State* L, DrawTarget& target, int head);
  // Log the tree of every head: one line per control with its class, name, rectangle, depth and state.
  void Dump() const;
  int CountControls() const;

  Control* focus = nullptr;                // keyboard focus
  Control* hover = nullptr;                // the control the cursor was last over (MouseEnter / MouseExit)
  int draggerRef = LUA_NOREF;              // the active dragger's script table (PostDragger), if any
  int draggerKey = 0;                      // the key code whose release ends it
  Control* engineDragger = nullptr;        // a control dragging by itself (an edit field's mouse selection)
  // The engine's own draggers (the original posts a C++ dragger object the same way scripts post
  // theirs): until the button `key` is let go, mouse moves go to control->DragMove and the release
  // to control->DragRelease. A script dragger that was active is cancelled.
  void PostEngineDragger(lua_State* L, Control* control, int key);
  std::vector<Control*> inputCapture;      // the capture stack; the last entry gets the input
  void Forget(Control* c);                 // a control died: drop it from focus and capture
  // Give the keyboard focus to a control or to none (MAUI_SetKeyboardFocus, FA exe 0x79cc10): the
  // control that had it until now hears self:OnKeyboardFocusChange(), also when it is given the
  // focus again; the new one hears nothing. `blocksKeyDown` is kept as the original keeps it
  // (what reads it is with the key maps).
  void SetFocus(lua_State* L, Control* c, bool blocksKeyDown = true);
  bool focusBlocksKeyDown = false;

 private:
  std::vector<Frame*> frames_;
  int width_ = 0, height_ = 0;
};

// The control behind a script value. CheckControl raises the original's errors: "Expected a game
// object...", "Game object has been destroyed", "Incorrect type of game object...".
template <class T = Control>
T* CheckControl(lua_State* L, int idx) {
  return CheckObject<T>(L, idx);
}

// One control's hold on another that may be destroyed first (a scrollbar and the control it
// scrolls): it keeps the other's script table and asks it for the control each time.
class WeakControl {
 public:
  WeakControl() = default;
  ~WeakControl() { Clear(); }
  WeakControl(const WeakControl&) = delete;
  WeakControl& operator=(const WeakControl&) = delete;
  void Set(lua_State* L, int idx);  // the control's table at idx
  void Clear();
  Control* Get() const;             // nullptr once the control is destroyed

 private:
  lua_State* L_ = nullptr;
  int ref_ = LUA_NOREF;
};

void RegisterControlBindings(lua_State* L);        // control.cpp: moho.control_methods, groups, plain controls
void RegisterFrameBindings(lua_State* L);          // ui_manager.cpp: frames, cursor, focus, capture
void RegisterBitmapBindings(lua_State* L);         // bitmap.cpp
void RegisterTextBindings(lua_State* L);           // text.cpp
void RegisterScrollbarBindings(lua_State* L);      // scrollbar.cpp
void RegisterItemListBindings(lua_State* L);       // itemlist.cpp
void RegisterEditBindings(lua_State* L);           // edit.cpp
void RegisterSimpleControlBindings(lua_State* L);  // simple_controls.cpp: border, histogram, movie
void RegisterInputBindings(lua_State* L);          // input.cpp: the host's input debugging functions
void RegisterMapPreviewBindings(lua_State* L);     // mappreview.cpp

// The parent argument of an Internal* creation function (stack index 2): must be a living control.
Control* CheckParent(lua_State* L);

}  // namespace moho::ui
