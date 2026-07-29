#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

// Correlates a character entity back to the AssignCharacterRequest that created
// it, so a later CancelAssignmentRequest can release exactly that entity.
//
// Only entities created by CharacterService::CreateCharacter carry this. The
// "already managed" branch of OnAssignCharacterRequest hands back a pre-existing
// world NPC and must never be destroyed by a cancellation, so it deliberately
// does not get one.
//
// The cookie is a client-local correlation id and is only unique per player,
// which is why cancellation matches on owner *and* cookie.
struct AssignmentCookieComponent
{
    AssignmentCookieComponent(uint32_t aCookie)
        : Cookie(aCookie)
    {
    }

    uint32_t Cookie;
};
