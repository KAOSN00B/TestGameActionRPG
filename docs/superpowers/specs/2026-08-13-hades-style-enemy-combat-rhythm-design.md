# Hades-Style Enemy Combat Rhythm Design

## Goal

Make normal encounters feel authored, readable, and varied without replacing the existing enemy roster, attacks, art, total room population, bosses, or encounter composition system.

The player should face a sequence of recognizable enemy decisions instead of a crowd that immediately homes toward them. Attack warnings remain clear, but movement and positioning become less predictable.

## Scope

This pass covers normal dungeon enemies and shared dungeon collision behavior:

- Limit simultaneous offensive pressure.
- Give uncommitted enemies useful positioning behavior.
- Stage reinforcements in smaller groups while preserving total population.
- Telegraph reinforcement locations with procedural purple circles and existing smoke VFX.
- Prevent ordinary enemy bodies from pushing the player into walls.
- Differentiate the positioning and engagement rhythm of the existing normal enemy roster.

Boss movesets, scripted prologue encounters, total room enemy counts, enemy artwork, and existing attack implementations remain intact. Bosses still receive the shared wall-safety correction where applicable.

## Current Root Causes

### Wall trapping

Player-versus-enemy capsule separation currently applies the full minimum translation vector to the player after tile and prop movement collision has already been resolved. Several enemies can therefore move the player repeatedly during one frame and leave the player overlapping solid room geometry. The next frame repeats the pressure, producing a wall trap.

### Horde-rush behavior

The encounter planner separates opening enemies from reinforcements, but room-capacity opening limits can reach 4, 6, 8, or 10 bodies. Reinforcement logic refills toward that opening count every six seconds or when active enemies fall to four. Most normal enemies independently move directly toward attack range, so enemies outside the existing attack slots still crowd the player.

### Weak enemy identity

Normal enemies have different attacks and role metadata, but their approach behavior largely collapses into two patterns: close distance for melee or maintain distance for a ranged attack. Their unique attacks do not create sufficiently distinct encounter rhythms.

## Combat Rhythm

Normal combat uses a shared engagement director. At any time, enemies are in one of three engagement intentions:

1. **Commit**: actively approach and perform an attack pattern.
2. **Support**: screen, hold a lane, protect an ally, or prepare a ranged action.
3. **Reposition**: circle, flank, retreat, or seek a better lane without closing directly.

The director grants a limited number of commit slots:

- Early rooms: 2 concurrent committers.
- Mid and late rooms: 3 concurrent committers.
- Swarm encounters may use one additional committer, but only for fragile swarm-profile enemies.

Attack-slot ownership is stable for the duration of a committed attack and its recovery. A waiting enemy cannot steal a slot every frame. After recovery, the enemy releases its slot and briefly repositions before it may request another turn.

Enemies that do not own a commit slot must not continuously home into the player's collision body. They choose a role-appropriate point around the player or an ally and maintain personal space.

## Reinforcement Staging

The encounter planner continues to generate the same total population and composition. Only simultaneous delivery changes.

- Standard reinforcement batches contain 1–2 enemies.
- A new batch begins only when the active body count is below the room's simultaneous target and no spawn telegraph batch is already pending.
- Timer expiration makes a batch eligible; it does not bypass the simultaneous target.
- Each batch reserves valid spawn positions before its telegraph begins.
- Reserved positions observe existing blocker checks and minimum player distance.
- Failed reservations stay queued and retry later; they do not spawn at an unsafe fallback.

Recommended simultaneous body targets are derived from room capacity:

- Small: 3
- Medium: 4
- Large: 5
- Arena: 6

These targets control bodies on screen, not the total enemies assigned to the room.

## Spawn Telegraph and VFX

Each reserved reinforcement position runs through a short state sequence:

1. **Circle warning (0.65 seconds):** a filled translucent purple raylib circle pulses at the exact spawn position, with one or two expanding outline rings.
2. **Smoke transition (0.30 seconds):** existing smoke particles burst at the position while the circle contracts and brightens.
3. **Spawn:** the enemy is instantiated at the reserved position and receives a short arrival/orientation delay before it may move or attack.

The full warning lasts approximately 0.95 seconds. The visual uses procedural raylib circles plus the existing VFX manager; it introduces no new bitmap asset.

Telegraphs render in world space below enemy sprites but above the room floor so players can route around them. Multiple warnings in a batch start together.

## Wall-Safe Physical Separation

Ordinary enemy bodies no longer push the player as the primary resolution response.

- When player and enemy capsules overlap, move the enemy outward by the collision translation vector.
- Validate the enemy's corrected position against room walls and props using the existing dungeon collision pass.
- If enemy-only correction cannot resolve a deep or malformed overlap, apply the smallest necessary correction to the player and immediately resolve that position against room tiles and props.
- Process collision corrections from a stable pre-resolution player position so multiple enemies cannot accumulate arbitrary pushes in one frame.
- Dashing continues to pass through enemies.
- Authored forced-push attacks remain separate mechanics and retain their intended knockback, subject to normal room collision.

This preserves physical presence while ensuring ordinary movement cannot pin the player against solid geometry.

## Enemy Identities

Existing attack implementations remain the source of damage. The rework changes approach, selection, commitment, and recovery behavior.

### Shadow Grunt

Circles at close-mid range, commits to a short strike, then disengages before requesting another turn. It is the readable baseline duelist.

### Slime

Advances slowly and directly as a space-taker. Once committed, it preserves its chosen line rather than continuously tracking every player movement.

### Skeleton Archer

Finds a clear firing lane, plants during its existing aim telegraph, fires, and relocates before firing again. It does not kite continuously.

### Flame Wisp

Prioritizes lateral positions and lane control. It maintains spacing from other ranged enemies so its fire pressure comes from a distinct angle.

### Sporeling

Approaches indirectly and prefers positions where its death cloud will restrict useful player space. It does not lead the initial rush.

### Shieldbearer

Selects a ranged or support ally to screen, occupying the line between that ally and the player. It commits offensively when its protected formation breaks or a slot is available.

### Phantom

Seeks an off-angle, commits to an ambush, then retreats or repositions after the attack. Its loop emphasizes disappearance and angle changes rather than constant pursuit.

### Bomber Imp

Searches for a useful throw lane, commits to a bomb action, then relocates. It avoids stacking directly with archers and wisps.

### Warchief

Anchors near the ally centroid and avoids becoming the nearest enemy unless isolated. Its presence changes the group's formation rather than adding another chaser.

### Living Blade

Uses fast pass-through commitments followed by a clear recovery and separation phase. It does not remain attached to the player after attacking.

## Architecture

The shared system belongs in the existing combat runtime rather than in the engine's main loop.

- `CombatDirector` calculates engagement intentions once per frame from active enemies, roles, player position, and current commitments.
- `Enemy` stores the assigned intention, positioning target, and stable commit ownership through its attack/recovery window.
- Individual enemy classes consume the shared intention and add their small identity-specific positioning rule.
- `EncounterPlanner` continues to own total composition.
- A bounded pending-spawn collection owned by `Engine` stages reserved reinforcement entries and their telegraph timers because spawning and rendering already route through the engine.
- `VFXManager` supplies the existing smoke burst; the engine draws the procedural warning circles in the dungeon world layer.
- Shared helper functions perform player/enemy separation without accumulating player displacement.

No new general-purpose AI framework or behavior-tree rewrite is introduced.

## State and Failure Handling

- Pending telegraphs are cleared on room exit, run reset, death, and editor-playtest reset.
- Pausing freezes telegraph timers with the rest of gameplay.
- A reserved spawn is revalidated immediately before instantiation. If invalid, it returns to the reinforcement queue.
- Dead, inactive, spawning, stunned, and dying enemies do not occupy commit slots.
- Enemies in an uninterruptible authored attack keep their slot until recovery or cancellation.
- If no valid reposition target exists, an enemy holds position rather than falling back to direct homing.

## Testing

Automated tests cover deterministic, non-rendering rules:

- Commit-slot limits by encounter tier and swarm status.
- Stable slot ownership during attack/recovery.
- Non-committers receive reposition/support intentions rather than direct pursuit.
- Reinforcement batches never exceed two entries.
- Simultaneous body targets are 3/4/5/6 by room-capacity band.
- Timer expiry cannot exceed the simultaneous target.
- Existing total planned population remains unchanged.
- Pending spawn lifecycle transitions from circle to smoke to ready.
- Invalid reserved positions return to the queue.
- Capsule separation favors enemy displacement and prevents accumulated player pushes.

Existing encounter planner, room capacity, combat, and collision tests must continue to pass. A desktop build verifies integration. Manual gameplay verification checks visual timing, enemy readability, wall escape, narrow rooms, swarm rooms, ranged-heavy groups, and room transitions during a pending spawn.

## Success Criteria

- Ordinary enemy contact cannot force the player into a wall or maintain a wall pin.
- No standard room exceeds its 3/4/5/6 simultaneous target through timed reinforcements.
- Total planned enemies per room remain unchanged.
- Reinforcements always show the purple-circle and smoke sequence before appearing.
- At most 2–3 normal enemies actively commit at once, except the explicit fragile-swarm allowance.
- Waiting enemies visibly reposition, screen, flank, or hold instead of converging on the player's body.
- Each listed normal enemy has a distinguishable approach/commit/recovery loop while retaining its existing attack and art.
