
#include "CutsceneManager.h"
#include "VirtualCanvas.h"
#include "raymath.h"
#include "VirtualCanvas.h"
#include <algorithm>
#include <cstring>

// ── Play ──────────────────────────────────────────────────────────────────────
void CutsceneManager::Play(const CutsceneAction* actions, int count)
{
    _actions             = actions;
    _count               = count;
    _current             = 0;
    _isActive            = true;
    _wantsAbilitySelect  = false;
    _wantsDoorUnlock     = false;
    _fadeAlpha           = 0.f;
    _visibleText.clear();

    // Ensure the dialogue box has its screen-relative defaults set once.
    _box.ApplyScreenDefaults();

    BeginCurrentAction(nullptr, nullptr);
}

void CutsceneManager::Stop()
{
    _isActive           = false;
    _wantsAbilitySelect = false;
    _wantsDoorUnlock    = false;
    _fadeAlpha          = 0.f;
    _visibleText.clear();
}

// ── BeginCurrentAction ────────────────────────────────────────────────────────
// Sets up the initial state for whatever action _current points to.
void CutsceneManager::BeginCurrentAction(Vector2* playerPos, Vector2* npcPos)
{
    if (_current >= _count)
    {
        _isActive = false;
        return;
    }

    const CutsceneAction& a = _actions[_current];

    switch (a.type)
    {
    case CutsceneAction::Type::FadeIn:
        _fadeAlpha    = 1.f;      // start fully black, fade to clear
        _fadeTimer    = 0.f;
        _fadeDuration = a.duration;
        _fadingIn     = true;
        break;

    case CutsceneAction::Type::FadeOut:
        _fadeAlpha    = 0.f;      // start clear, fade to black
        _fadeTimer    = 0.f;
        _fadeDuration = a.duration;
        _fadingIn     = false;
        break;

    case CutsceneAction::Type::Dialogue:
        // Compute page breaks for the full text against the current box layout.
        _pageBreaks = _box.ComputePageBreaks(a.text ? a.text : "");
        _pageIdx    = 0;
        _pageStart  = 0;
        _pageLength = _pageBreaks.empty() ? 0 : _pageBreaks[0];

        _visibleText.clear();
        _typewriterTimer = 0.f;
        _waitingForInput = false;
        _blinkTimer      = 0.f;
        break;

    case CutsceneAction::Type::Wait:
        _waitTimer = a.duration;
        break;

    case CutsceneAction::Type::MoveActor:
        _moveActorIdx = a.actorIdx;
        _moveTarget   = a.target;
        _moveTimer    = 0.f;
        _moveDuration = a.duration;
        _moveCaptured = false;   // start position captured on first Update call
        break;

    case CutsceneAction::Type::OpenAbilitySelect:
        // Set the flag; Engine will detect it and open the ability screen.
        // Cutscene sits here until OnAbilitySelected() is called.
        _wantsAbilitySelect = true;
        break;

    case CutsceneAction::Type::UnlockDoor:
        // Flag is instant — Engine consumes it and we advance immediately.
        _wantsDoorUnlock = true;
        Advance(playerPos, npcPos);
        break;

    case CutsceneAction::Type::EndCutscene:
        _isActive = false;
        break;
    }
}

// ── Advance ───────────────────────────────────────────────────────────────────
void CutsceneManager::Advance(Vector2* playerPos, Vector2* npcPos)
{
    _current++;
    BeginCurrentAction(playerPos, npcPos);
}

// ── Update ────────────────────────────────────────────────────────────────────
void CutsceneManager::Update(float dt, Vector2* playerPos, Vector2* npcPos)
{
    if (!_isActive) return;

    _blinkTimer += dt;

    const CutsceneAction& a = _actions[_current];

    switch (a.type)
    {
    // ── FadeIn / FadeOut ──────────────────────────────────────────────────────
    case CutsceneAction::Type::FadeIn:
    case CutsceneAction::Type::FadeOut:
    {
        _fadeTimer += dt;
        float t = (_fadeDuration > 0.f)
            ? std::min(_fadeTimer / _fadeDuration, 1.f)
            : 1.f;
        _fadeAlpha = _fadingIn ? (1.f - t) : t;
        if (_fadeTimer >= _fadeDuration)
            Advance(playerPos, npcPos);
        break;
    }

    // ── Dialogue ──────────────────────────────────────────────────────────────
    case CutsceneAction::Type::Dialogue:
    {
        if (_waitingForInput) break;   // page fully shown — waiting for E press

        // Reveal chars on the current page only; timer resets to 0 each new page.
        _typewriterTimer += dt * kCharsPerSecond;
        int revealedOnPage = std::min((int)_typewriterTimer, _pageLength);

        if (a.text != nullptr)
            _visibleText = std::string(a.text + _pageStart, revealedOnPage);

        if (revealedOnPage >= _pageLength)
            _waitingForInput = true;

        break;
    }

    // ── Wait ──────────────────────────────────────────────────────────────────
    case CutsceneAction::Type::Wait:
        _waitTimer -= dt;
        if (_waitTimer <= 0.f)
            Advance(playerPos, npcPos);
        break;

    // ── MoveActor ─────────────────────────────────────────────────────────────
    case CutsceneAction::Type::MoveActor:
    {
        Vector2* actorPos = (_moveActorIdx == 0) ? playerPos : npcPos;

        // Snap the start position on the first frame of movement.
        if (!_moveCaptured)
        {
            _moveOrigin   = actorPos ? *actorPos : Vector2Zero();
            _moveCaptured = true;
        }

        _moveTimer += dt;
        float t = (_moveDuration > 0.f)
            ? std::min(_moveTimer / _moveDuration, 1.f)
            : 1.f;

        // Smooth ease-in-out curve (no sudden start or stop).
        float ease = t * t * (3.f - 2.f * t);

        if (actorPos)
            *actorPos = Vector2Lerp(_moveOrigin, _moveTarget, ease);

        if (_moveTimer >= _moveDuration)
            Advance(playerPos, npcPos);
        break;
    }

    // ── OpenAbilitySelect ─────────────────────────────────────────────────────
    case CutsceneAction::Type::OpenAbilitySelect:
        // Stays here; Engine detects WantsAbilitySelect() and calls
        // OnAbilitySelected() after the player makes their choice.
        break;

    default:
        break;
    }
}

// ── AdvanceOnInput ────────────────────────────────────────────────────────────
// Called by Engine when the player presses E or left-clicks during a Dialogue.
// First press: skip the typewriter to show the whole current page.
// Second press on the last page: advance to the next cutscene action.
// Second press on a non-last page: flip to the next page and restart typewriter.
void CutsceneManager::AdvanceOnInput()
{
    if (!_isActive) return;
    if (_current >= _count) return;

    const CutsceneAction& a = _actions[_current];
    if (a.type != CutsceneAction::Type::Dialogue) return;

    if (!_waitingForInput)
    {
        // Skip typewriter — reveal the entire current page immediately.
        _typewriterTimer = (float)_pageLength;
        if (a.text) _visibleText = std::string(a.text + _pageStart, _pageLength);
        _waitingForInput = true;
    }
    else if (_pageIdx + 1 < (int)_pageBreaks.size())
    {
        // More pages remain — advance to the next page.
        _pageStart  = _pageBreaks[_pageIdx];
        _pageIdx++;
        _pageLength      = _pageBreaks[_pageIdx] - _pageStart;
        _typewriterTimer = 0.f;
        _visibleText.clear();
        _waitingForInput = false;
    }
    else
    {
        // Last page fully shown — move to the next cutscene action.
        Advance(nullptr, nullptr);
    }
}

// ── OnAbilitySelected ─────────────────────────────────────────────────────────
void CutsceneManager::OnAbilitySelected()
{
    _wantsAbilitySelect = false;
    Advance(nullptr, nullptr);
}

// ── Draw ──────────────────────────────────────────────────────────────────────
void CutsceneManager::Draw(Texture2D borderTex, Texture2D portraitTex, InputPromptMode promptMode) const
{
    if (!_isActive) return;

    // ── Fade overlay (always drawn if there is any opacity) ───────────────────
    if (_fadeAlpha > 0.01f)
    {
        DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight,
            Fade(BLACK, _fadeAlpha));
    }

    // ── Dialogue box (only during Dialogue actions) ───────────────────────────
    if (_current < _count &&
        _actions[_current].type == CutsceneAction::Type::Dialogue)
    {
        // Blink the continue prompt at ~2 Hz when fully revealed.
        bool showContinue = _waitingForInput && (sinf(_blinkTimer * 6.f) > 0.f);

        const char* speaker = _actions[_current].speaker;
        _box.Draw(borderTex, portraitTex, speaker, _visibleText, showContinue, promptMode);
    }
}
