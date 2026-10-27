#pragma once

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/transport_authority.hpp>

namespace mediaserver::app {

// Applies one operator command to a transport - the live show's or a
// Director-local Preview's - re-anchoring it at the control frame the result
// will be sealed into. Every command is exact integer state, and the nodes
// learn about a live one through the ordinary sealed-frame path.
//
// Play, from the toggle or L, always runs forwards; J runs backwards. Either
// turns a show already playing the other way round where it stands, and K
// stops it where it stands. Stepping and jumping pause first.
void apply_command(show::TransportAuthority &transport,
                   show::PresenterCommand command,
                   cmd::FrameNumber at) noexcept;

} // namespace mediaserver::app
