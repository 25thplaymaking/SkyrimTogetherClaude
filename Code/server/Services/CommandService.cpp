#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
}

namespace
{
// Authorises the *authenticated* sender.
//
// The identity must come from acMessage.pPlayer, which the server resolved from
// the connection, and never from a PlayerId carried inside the packet -- that
// field is entirely attacker-controlled, so trusting it let any client execute
// admin commands by naming an admin's id.
//
// GetByConnectionId may return nullptr for a session that has gone away, so the
// lookup is null-checked; dereferencing it unchecked crashed the server.
bool IsAdmin(const Player* apPlayer) noexcept
{
    if (!apPlayer)
        return false;

    for (const auto session : GameServer::Get()->GetAdminSessions())
    {
        const Player* pAdmin = PlayerManager::Get()->GetByConnectionId(session);
        if (pAdmin && pAdmin->GetId() == apPlayer->GetId())
            return true;
    }

    return false;
}
} // namespace

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    NotifySetTimeResult response{};

    if (!IsAdmin(acMessage.pPlayer))
    {
        response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
        acMessage.pPlayer->Send(response);
        return;
    }

    const auto cHours = static_cast<int>(acMessage.Packet.Hours);
    const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

    m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale());

    response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
    acMessage.pPlayer->Send(response);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    // This handler had no permission check at all: it returned any named
    // player's exact position, cell and worldspace to any requester. An empty
    // response is already what "target not found" looks like, so a denial is
    // indistinguishable from a miss and discloses nothing.
    if (!IsAdmin(acMessage.pPlayer))
    {
        TeleportCommandResponse denied{};
        acMessage.pPlayer->Send(denied);
        return;
    }

    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};
    if (pTargetPlayer)
    {
        auto character = pTargetPlayer->GetCharacter();
        if (character)
        {
            const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
            if (pMovementComponent)
            {
                const auto& cellComponent = pTargetPlayer->GetCellComponent();
                response.CellId = cellComponent.Cell;
                response.Position = pMovementComponent->Position;
                response.WorldSpaceId = cellComponent.WorldSpaceId;
            }
        }
    }

    acMessage.pPlayer->Send(response);
}
