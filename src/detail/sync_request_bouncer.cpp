//---------------------------------------------------------------------------//
// Copyright (c) 2011-2018 Dominik Charousset
// Copyright (c) 2017-2020 Mikhail Komarov <nemo@nil.foundation>
//
// Distributed under the terms and conditions of the BSD 3-Clause License or
// (at your option) under the terms and conditions of the Boost Software
// License 1.0. See accompanying files LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt.
//---------------------------------------------------------------------------//

#include <nil/actor/actor.hpp>
#include <nil/actor/actor_cast.hpp>
#include <nil/actor/config.hpp>
#include <nil/actor/exit_reason.hpp>
#include <nil/actor/mailbox_element.hpp>
#include <nil/actor/message_id.hpp>
#include <nil/actor/sec.hpp>
#include <nil/actor/system_messages.hpp>

#include <nil/actor/detail/sync_request_bouncer.hpp>

namespace nil::actor::detail {

    sync_request_bouncer::sync_request_bouncer(error r) : rsn(std::move(r)) {
        // nop
    }

    void sync_request_bouncer::operator()(const strong_actor_ptr &sender, const message_id &mid) const {
        if (sender && mid.is_request())
            sender->enqueue(nullptr, mid.response_id(), make_message(make_error(sec::request_receiver_down)),
                            // TODO: this breaks out of the execution unit
                            nullptr);
    }

    void sync_request_bouncer::operator()(const mailbox_element &e) const {
        (*this)(e.sender, e.mid);
    }

}    // namespace nil::actor::detail