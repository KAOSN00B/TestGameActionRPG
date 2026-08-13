#include "EncounterPlanner.h"

#include <cassert>

namespace
{
    RoomCombatCapacity Capacity(RoomCapacityBand band)
    {
        RoomLayout room{};
        room.combatCapacityOverride = static_cast<RoomCapacityOverride>((int)band);
        return RoomCapacityAnalyzer::Analyze(room, TileDefSet{});
    }
}

int main()
{
    EncounterRequest tight{};
    tight.seed = 41;
    tight.tier = 2;
    tight.profile = EncounterProfile::Assault;
    tight.capacity = Capacity(RoomCapacityBand::Small);
    tight.learnedAbilityCount = 5;
    const EncounterPlan tightPlan = EncounterPlanner::Build(tight);
    assert((int)tightPlan.opening.size() <= 4);
    assert(tightPlan.debug.plannedPopulation <= 7);
    assert(tightPlan.debug.expensiveUnits <= 1);
    // Task 4 (Reinforcement Pacing): the planner's opening/reinforcements split
    // must always account for the full planned population — Engine.cpp's Task 4
    // change only re-partitions HOW MANY of plan.opening spawn immediately vs.
    // get queued as pending reservations; it never adds/drops planner output,
    // so this invariant holding here is sufficient (every populationCost in
    // this table is 1, so entry count == plannedPopulation exactly).
    assert((int)tightPlan.opening.size() + (int)tightPlan.reinforcements.size()
           == tightPlan.debug.plannedPopulation);

    EncounterRequest large{};
    large.seed = 72;
    large.tier = 1;
    large.profile = EncounterProfile::Assault;
    large.capacity = Capacity(RoomCapacityBand::Large);
    const EncounterPlan fewerAbilities = EncounterPlanner::Build(large);
    large.learnedAbilityCount = 4;
    const EncounterPlan moreAbilities = EncounterPlanner::Build(large);
    assert(moreAbilities.debug.plannedPopulation >= fewerAbilities.debug.plannedPopulation);
    assert(moreAbilities.debug.plannedPopulation <= 13);
    assert((int)moreAbilities.opening.size() <= 8);
    assert((int)fewerAbilities.opening.size() + (int)fewerAbilities.reinforcements.size()
           == fewerAbilities.debug.plannedPopulation);
    assert((int)moreAbilities.opening.size() + (int)moreAbilities.reinforcements.size()
           == moreAbilities.debug.plannedPopulation);

    EncounterRequest swarm{};
    swarm.seed = 99;
    swarm.tier = 2;
    swarm.profile = EncounterProfile::Swarm;
    swarm.swarm = true;
    swarm.capacity = Capacity(RoomCapacityBand::Arena);
    const EncounterPlan swarmPlan = EncounterPlanner::Build(swarm);
    assert(swarmPlan.debug.plannedPopulation >= 14);
    assert(swarmPlan.debug.plannedPopulation <= 18);
    assert((int)swarmPlan.opening.size() <= 10);
    assert(!swarmPlan.reinforcements.empty());
    assert((int)swarmPlan.opening.size() + (int)swarmPlan.reinforcements.size()
           == swarmPlan.debug.plannedPopulation);

    return 0;
}
