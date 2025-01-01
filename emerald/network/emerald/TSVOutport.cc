/*
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "mem/ruby/network/emerald/TSVOutport.hh"

#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/emerald/Affirm.hh"
#include "mem/ruby/network/emerald/AffirmLink.hh"
#include "mem/ruby/network/emerald/TSV.hh"
#include "mem/ruby/network/emerald/fragmentBuffer.hh"

//=====================================
#include <iostream>
//=====================================

namespace gem5
{

namespace ruby
{

namespace emerald
{

//TSVOutputUnit constructor for instantiation
TSVOutport::TSVOutport(int id, PortDirection direction, TSV *bus,
  uint32_t consumerVcs)
  : Consumer(bus), m_bus(bus), m_id(id), m_direction(direction),
    m_vc_per_vnet(consumerVcs)
{
    //number of VCs in this OutputUnit
    const int m_num_vcs = consumerVcs * m_bus->get_num_vnets();
    //preallocate memory for m_num_vcs elements in outVcState vector
    //for each VC, we need to save its state in outVcState vector
    outVcState.reserve(m_num_vcs);
    //Instantiating outVcState vector
    for (int i = 0; i < m_num_vcs; i++) {
        //initialize a VcState object
        outVcState.emplace_back(i, m_bus->get_net_ptr(), consumerVcs);
    }
}

//for decrementing the credit in the appropriate output VC
void
TSVOutport::decrement_credit(int out_vc)
{
    //printing router_id, the OutputUnit, outvc credits, outvc,
    //current cycle, and credit_link
    DPRINTF(RubyNetwork, "TSV %d OutputUnit %s decrementing credit:%d for "
            "outvc %d at time: %lld for %s\n", m_bus->get_id(),
            m_bus->getPortDirectionName(get_direction()),
            outVcState[out_vc].get_credit_count(),
            out_vc, m_bus->curCycle(), m_credit_link->name());

    //decrement credit for out_vc
    outVcState[out_vc].decrement_credit();
}

//for incrementing the credit in the appropriate output VC
void
TSVOutport::increment_credit(int out_vc)
{
    //printing router_id, the OutputUnit, outvc credits, outvc,
    //current cycle, and credit_link
    DPRINTF(RubyNetwork, "TSV %d OutputUnit %s incrementing credit:%d for "
            "outvc %d at time: %lld from:%s\n", m_bus->get_id(),
            m_bus->getPortDirectionName(get_direction()),
            outVcState[out_vc].get_credit_count(),
            out_vc, m_bus->curCycle(), m_credit_link->name());

    //increment credit for out_vc
    outVcState[out_vc].increment_credit();
}

// Check if the output VC (i.e., input VC at next router)
// has free credits (i..e, buffer slots).
// This is tracked by OutVcState
bool
TSVOutport::has_credit(int out_vc)
{
    //make sure out_vc state is ACTIVE_
    assert(outVcState[out_vc].isInState(ACTIVE_, curTick()));
    return outVcState[out_vc].has_credit();
}


// Check if the output port (i.e., input port at next router) has free VCs.
bool
TSVOutport::has_free_vc(int vnet)
{
    //the first VC in the given Vnet
    int vc_base = vnet*m_vc_per_vnet;
    //go through all VCs in the given Vnet, if you found a VC that
    //is in IDLE_ state, then we have a free VC
    for (int vc = vc_base; vc < vc_base + m_vc_per_vnet; vc++) {
        if (is_vc_idle(vc, curTick()))
            return true;
    }

    return false;
}

// Assign a free output VC to the winner of Switch Allocation
int
TSVOutport::select_free_vc(int vnet)
{
    //the first VC in the given Vnet
    int vc_base = vnet*m_vc_per_vnet;
    //go through all VCs in the given Vnet, find the first VC that
    //is in IDLE_ state (that VC is free), change its state to ACTIVE_,
    //and assign that free outvc to the winner of SA
    for (int vc = vc_base; vc < vc_base + m_vc_per_vnet; vc++) {
        if (is_vc_idle(vc, curTick())) {
            outVcState[vc].setState(ACTIVE_, curTick());
            return vc;
        }
    }
    //it returns -1 if we can't find a free VC in the outport
    return -1;
}

/*
 * The wakeup function of the OutputUnit reads the credit signal from the
 * downstream router for the output VC (i.e., input VC at downstream router).
 * It increments the credit count in the appropriate output VC state.
 * If the credit carries is_free_signal as true,
 * the output VC is marked IDLE (meaning that VC is free).
 */
void
TSVOutport::wakeup()
{
    //if the credit link of the outport is ready at the current tick
    if (m_credit_link->isReady(curTick())) {
        //put the content of the credit link into t_credit
        Affirm *t_credit = (Affirm*) m_credit_link->consumeLink();
        //increment the credit for the outvc of t_credit
        //It means that outvc (i.e., input VC of the downstream router)
        //has one more free slot.
        increment_credit(t_credit->get_vc());

        //if is_free_signal in t_credit is true, then set the VC state
        //for that outvc to IDLE_
        if (t_credit->is_free_signal())
            set_vc_state(IDLE_, t_credit->get_vc(), curTick());

        //deleting the created variable
        delete t_credit;

        //if the credit link of the outport is ready at the current tick
        if (m_credit_link->isReady(curTick())) {
            //schedule the consumption event for the next cycle
            scheduleEvent(Cycles(1));
        }
    }
}

//get the OutputUnit network queue (buffer that sends the flits
//on the output (network) link, to a VC in downstream router InputUnit)
fragmentBuffer*
TSVOutport::getOutQueue()
{
    return &outBuffer;
}

//set the output (network) link for the OutputUnit
void
TSVOutport::set_out_link(GridLink *link)
{
    m_out_link = link;
}

//set the credit link for the OutputUnit
void
TSVOutport::set_credit_link(AffirmLink *credit_link)
{
    m_credit_link = credit_link;
}

//for inserting a flit into an output VC
//(i.e., input VC of the downstream router)
void
TSVOutport::insert_flit(fragment *t_flit)
{
    //insert t_flit into outBuffer flitBuffer
    outBuffer.insert(t_flit);
    //======================================================
    // std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
    // std::cout << "t_flit entered the outBuffer of the bus outport (TSVOutputUnit.cc).\n";
    // std::cout << "ID of the t_flit in outBuffer: " << t_flit->get_id() <<"\n";
    // std::cout << "t_flit source router is: R" << t_flit->get_route().src_router <<"\n";
    // std::cout << "t_flit destination router is: R" << t_flit->get_route().dest_router <<"\n";
    // // flit *top_flit = outBuffer.peekTopFlit();
    // // std::cout << "ID of the flit at the top of the outBuffer is: " << top_flit->get_id() <<"\n";
    // std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
    //======================================================
    //schedule consumption event for m_out_link for the next cycle
    // m_out_link->scheduleEventAbsolute(m_bus->clockEdge(Cycles(1)));
    m_out_link->scheduleEventAbsolute(m_bus->clockEdge(Cycles(0)));
}

bool
TSVOutport::functionalRead(Packet *pkt, WriteMask &mask)
{
    return outBuffer.functionalRead(pkt, mask);
}

//updating outBuffer flits with the data from the packet
uint32_t
TSVOutport::functionalWrite(Packet *pkt)
{
    return outBuffer.functionalWrite(pkt);
}

} // namespace emerald
} // namespace ruby
} // namespace gem5
