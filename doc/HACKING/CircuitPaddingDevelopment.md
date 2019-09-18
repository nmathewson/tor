{:toc}

# Circuit Padding Developer Documentation

This document is written for researchers who wish to prototype and evaluate circuit-level padding defenses in Tor.

Written by Mike Perry and George Kadianakis.

## 0. Background

Tor has deployed both connection-level and circuit-level padding, and both systems are live on the network today. The connection-level padding behavior is described in section 2 of [padding-spec.txt](https://github.com/mikeperry-tor/torspec/blob/hs-padding-spec/padding-spec.txt). The circuit-level padding behavior is described in section 3 of padding-spec.txt.

These two systems are orthogonal and should not be confused. The connection-level padding system regards circuit-level padding as normal data traffic, and hence the connection-level padding system will not add any additional overhead while the circuit-level padding system is actively padding.

While the currently deployed circuit-level padding behavior is quite simplistic, it is implemented using only a few of the features of a comprehensive event-driven state machine framework. The framework is designed to support all of the features needed to deploy any delay-free statistically shaped cover traffic on individual circuits, with cover traffic flowing to and from a node of the implementor's choice (Guard, Middle, Exit, Rendezvous, etc).

This class of system was first proposed in [Timing analysis in low-latency mix networks:
attacks and defenses](https://www.freehaven.net/anonbib/cache/ShWa-Timing06.pdf) by Shmatikov and Wang, and extended for the website traffic fingerprinting domain by Juarez et al in [Toward an Efficient Website Fingerprinting Defense](http://arxiv.org/pdf/1512.00524). The framework also has support for fixed parameterized probability distributions, as used in [APE](https://www.cs.kau.se/pulls/hot/thebasketcase-ape/) by Tobias Pulls, as well as other additional features.

This document describes how to use Tor's circuit padding framework to implement and deploy novel delay-free cover traffic defenses.

## 1. Circuit Padding Framework Introduction

The circuit padding framework is the official way to implement padding defenses in Tor. It may be used in combination with application-layer defenses, or on its own.

It is meant to be comprehensive enough to deploy most defenses without modification, but it may be extended to provide new features as well. Section 2XXX of this document describes how to use the framework as-is to add a new padding machine, and Section 3 covers internal topics that will help you if you wish to add new framework features to support your machine.

### 1.1. Global System Overview

To start off, circuit-level padding occurs between Tor clients and target relays at any hop of the circuit. Both parties need to support the same padding mechanisms for the system to work, but only the client can enable it: there is a padding negotiation mechanism in the Tor protocol that clients use to ask a relay to start doing padding. Padding mechanisms are currently hardcoded in the Tor source code, but in the future we will be able to serialize them in the Tor consensus or in Tor configuration files.

Circuit-level padding is specified using 'padding machines'. Padding machines are in principle ''finite state machines''' whose every state specifies a different form of padding style, or stage of padding, in terms of latency and throughput. The padding designer specifies under which circumstances the padding machine should transition between padding states, either based on external events (e.g. incoming Tor cells) or on internal events (e.g. the state of machine, packet counts, etc).

There are certain circuit-level conditions (e.g. the circuit is of a certain type) that can be specified by the padding designer under which the padding machine will get activated. When a padding machine gets activated, it gets attached to the corresponding circuit both on the client-side and the relay-side. The machine will continue to run so long as its circuit-level conditions hold, or until it reaches a terminal end state.

The following sections cover the details of the engineering steps that need to be performed to write, test, and deploy a padding machine, as well as how to extend the framework to support new machine features.

### 1.2. Design Philosophy and Design Constraints

The circuit padding framework is designed to allow arbitrary forms of cover
traffic to be added to Tor circuits, for a wide range of purposes and
scenarios. Through [proper load
balancing](https://gitweb.torproject.org/torspec.git/tree/proposals/265-load-balancing-with-overhead.txt)
and [circuit multiplexing
strategies](https://trac.torproject.org/projects/tor/ticket/29494), we believe
it is possible to add significant bandwidth overhead in the form of cover
traffic, without significantly impacting end-user performance.

Notably absent from the framework, though, is the ability to add delay to
circuit traffic. We are keenly aware that if we were to support additional
delay, [defenses would be able to have more success with less bandwidth
overhead][https://freedom.cs.purdue.edu/anonymity/trilemma/index.html].

Even so, arbitrary delay is not supported by this framework for two major
reasons:

First: Arbitrary delay inherently adds queueing memory overhead to all relays.
Tor's static flow control is not sufficiently tuned to rapidly react to
injected delays, and it is unlikely that even a properly tuned dynamic flow
control mechanism would be able to keep arbitrary delay queuing overhead
bounded to a constant amount of memory overhead, as the network scales with more
relays and more users.

Second: Additional latency is unappealing to the wider Internet community, for
the simple reason that bandwidth
[https://ipcarrier.blogspot.com/2014/02/bandwidth-growth-nearly-what-one-would.html](continues
to increase exponentially), where as the speed of light is fixed. Significant
engineering effort has been devoted to optimizations that reduce the effect of
latency on Internet protocols. To go against this trend would ensure our
irellevance to the wider conversation about traffic analysis of low latency
protocols.

However, these two factors only mean that delay is unacceptable *to add to the
general-purpose Tor network*. It does not mean that delay is unacceptable in
every case. It is acceptable for specific websites to add delay to themselves
and even to signal delay strategies to their own clients, such as in the
[https://petsymposium.org/2017/papers/issue2/paper54-2017-2-source.pdf](Alpaca
defense), since this does not affect the Tor network, nor does it apply to
other websites which may prefer responsiveness. It is also acceptable
for high-latency protocols to utilize a fixed-size batch mixing stragegy
inside the Tor network, as fixed-size batch mixing strategies keep memory
overhead constant, and priority mechanisms could be used to ensure batch mixed
traffic has no imact on low latency traffic.

Unfortunately, this still means that client-side web browser defenses that add
delay to all web clients (such as LLaMa and Walkie-Talkie), are also
unacceptable, *unless their use can be negotiated only by specific websites*.


## 2. Creating a New Padding Machine

You must perform three pieces of work to create and enable a new padding machine:
  1. Define your machine using the fields of a heap-allocated `circpad_machine_spec_t` structure instance.
  2. Register your instance in the global lists of available padding machines using `circpad_register_padding_machine()`.
  3. Ensure that your machine is properly negotiated under your desired circuit conditions.

### 2.1. Creating and Registering a New Padding Machine

Your machine definitions should go into their own functions in [circuitpadding_machines.c](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding_machines.c). For details on all of the fields involved in specifying a padding machine, see Section XXX.

You must register your machine in `circpad_machines_init()` in [circuitpadding.c](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding.c). When adding a new padding machine specification, you must allocate a `circpad_machine_spec_t` on the heap with `tor_malloc_zero()`, give it a human readable name string, and a machine number equivalent to the number of machines in the list, and register the structure using `circpad_register_padding_machine()`.

XXX: Right now, our machine registration actually happens in our specification definition functions. This is confusing. We should move the registration calls into `circpad_machines_init()` instead of the specification definition functions..

Each machine must have a client instance, and a relay instance. Register your client-side machine instance in the `origin_padding_machines` list, and your relay side machine instance in the `relay_padding_machines` list. You do not need to worry about deallocation; this is handled for you automatically.

Both machine lists use registration order to signal machine precidence for a given `machine_idx` slot on a circuit. This means that machines that are registered last are checked for activation *before* machines that were registered first. (This reverse precidence ordering allows us to easily deprecate older machines simply by adding new ones after them.)

### 2.2. Per-Circuit Machine Activation and Shutdown

After a machine has been successfully registered with the framework, it will be instantiated on any client-side circuits that support it. Only client-side circuits may initiate padding machines, but either clients or relays may shut down padding machines.

#### 2.2.1. Machine Application Events

The framework checks client-side origin circuits to see if padding machines should be activated or terminated during specific event callbacks in circuitpadding.c. We list these event callbacks here only for reference. You should not modify any of these callbacks to get your machine to run; instead, you should use the `circpad_machine_spec_t.conditions` field, which is described in the next section. However, you may add new event callbacks if you need other activation events. Any new event callbacks should behave exactly like the existing callbacks.

During each of these event callbacks, the framework checks to see if any current running padding machines have conditions that no longer apply as a result of the event, and shuts those machines down. Then, it checks to see if any new padding machines should be activated as a result of the event, based on their circuit application conditions. **Remember: Machines are checked in reverse order in the machine list. This means that later, more recently added machines take precidence over older, earlier entries in each list.**

Both of these checks are performed using the machine application conditions that you specify in your machine's `circpad_machine_spec_t.conditions` field.

The machine application event callbacks are prefixed by `circpad_machine_event_` by convention in circuitpadding.c. As of this writing, these callbacks are:

  - `circpad_machine_event_circ_added_hop()`: Called whenever a new hop is added to a circuit.
  - `circpad_machine_event_circ_built()`: Called when a circuit has completed consturction and is opened.
  - `circpad_machine_event_circ_purpose_changed()`: Called when a circuit changes purpose.
  - `circpad_machine_event_circ_has_no_relay_early()`: Called when a circuit runs out of RELAY_EARLY cells.
  - `circpad_machine_event_circ_has_streams()`: Called when a circuit gets a stream attached.
  - `circpad_machine_event_circ_has_no_streams()`: Called when the last stream is detached from a circuit.

XXX perhaps instead of the callbacks we need to specify the conditions? In theory the padding designer should not care about the callbacks since that's part of the internals of the framework. The conditions are talked about in the section below.

#### 2.2.2. Machine Application Conditions

The [circpad_machine_conditions_t conditions field](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L641) of your `circpad_machine_spec_t` machine definition instance controls the conditions under which your machine gets attached and enabled on a Tor circuit, and when it gets shut down.

*All* of your explicitly specified conditions in `circpad_machine_spec_t.conditions` *must* be met for the machine to be applied to a circuit. If *any* condition that you specified ceases to be met upon the arrival of one of the machine activation events, then the machine is shut down (even if the event is unrelated to that condition; all conditions are checked on each event). Another way to look at this is that all specified conditions must evaluate to true for the entire duration that your machine is running. If any are false, your machine does not run (or stops running and shuts down).

In particular, as part of the [circpad_machine_conditions_t structure](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding.h#L149), the circuit padding subsystem gives the developer the option to enable a machine based on:
  - The [number of hops](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L157) on the circuit (via the `min_hops` field).
  - The [current state](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L174) of the circuit, such as streams, relay_early, etc (via the `circpad_circuit_state_t state_mask` field).
  - The [purpose](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L178) (i.e. type) of the circuit (via the `circpad_purpose_mask_t purpose_mask` field).

This condition mechanism is the preferred way to determine if a machine should apply to a circuit. For information about potentially useful conditions that we have considered but have not yet implemented, see Section XXX. We will happily accept patches for those conditions, or any for other additional conditions that are needed for your use case.

#### 2.2.3. Detecting and Negotiating Machine Support

When a new machine specification is added to Tor (or removed from Tor), you should bump the Padding subprotocol version in ./src/core/or/protover.c and ./src/rust/protover/protover.rs, add a field to protover_summary_flags_t in or.h, and set this field in memoize_protover_summary() in versions.c. This new field must then be checked in circpad_node_supports_padding() in circuitpadding.c. XXX-link all of these.

Note that this protocol version update is not necessary if your experiments will *only* be using your own relays that support your own padding machines. This can be accomplished by using the `MiddleNodes` directive; see Section XXX for more information.

If the protocol support check passes for the circuit, then the client sends a RELAY_COMMAND_PADDING_NEGOTIATE cell towards the `circpad_machine_spec_t.target_hop` relay, and immediately enables the padding machine, and may begin sending padding. (The framework does not wait for RELAY_COMMAND_PADDING_NEGOTIATED response to begin padding so that we can switch between machines rapidly.)
 
#### 2.2.4. Machine Shutdown Mechanisms

Padding machines can be shut down on a circuit in three main ways:
  1. During a `circpad_machine_event` callback, when `circpad_machine_spec_t.conditions` no longer applies (client side)
  2. After a transition to the CIRCPAD_STATE_END, if `circpad_machine_spec_t.should_negotiate_end` is set (client or relay side)
  3. If there is a RELAY_COMMAND_PADDING_NEGOTIATED error response from the relay during negotiation.

Each of these cases causes the originating node to send a relay cell towards the other side, indicating that shutown has occurred. The client side sends RELAY_COMMAND_PADDING_NEGOTIATE, and the relay side sends RELAY_COMMAND_PADDING_NEGOTIATED.

Because padding from malicious Exit nodes can be used to construct active timing-based side channels to malicious Guard nodes, the client checks that padding-relatd cells only come from relays with active padding machines on them. For this reason, when a client decides to shut down a padding machine, the framework frees the mutable `circuit_t.padding_info`, but leaves the `circuit_t.padding_machine` pointer set until the RELAY_COMMAND_PADDING_NEGOTIATED response comes back, to ensure that any remaining in-flight padding packets are recognized as being valid. Tor does not yet close circuits due to violation of this property, but the [vanguards addon bandguard component](https://github.com/mikeperry-tor/vanguards/blob/master/README_TECHNICAL.md#the-bandguards-subsystem) does.

As an optimization, a client is allowed to replace a machine with another, by sending a RELAY_COMMAND_PADDING_NEGOTIATE cell to shut down a machine, and immediately sending a RELAY_COMMAND_PADDING_NEGOTIATE to start a new machine in the same index, without waiting for the response from the first negotiate cell.

Unfortunately, there is a known bug as a consequence of this optimization. If your machine depends on repeated, rapid shutdown and startup (either for itself or between multiple different machines), please see [Bug 30922](https://trac.torproject.org/projects/tor/ticket/30992). Depending on your use case, we may need to fix that bug or help you find a workaround.


## 3. Padding Machine Definition Details

By now, you should understand how to register, negotiate, and control the lifetime of your padding machine. This section should help you understand the full set of features available for your padding machine definitions.

A padding machine is specified using the [circpad_machine_spec_t structure](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L605). Instances of this structure specify the precise functionality of a machine, and it's what the circuit padding developer is called to write. Instances of structure are created only at startup, and are referenced via `const` pointers during normal operation.

In this section we will go through the most important elements of that structure.

### 3.1 Padding Machine States

A padding machine is basically a finite state machine where each state specifies a different style of padding. Transitions between states occur using various types of events as specified in XXX.

As an example of a simple padding machine, you could have a state machine with the following states: `[START] -> [SETUP] -> [HTTP] -> [END]` where the `[SETUP]` state pads in a way that obfuscates the ''circuit setup'' of Tor, and the `[HTTP]` state pads in a way that emulates a simple HTTP session. Of course, padding machines can be more complicated than that, with dozens of states and non-trivial transitions.

Padding developers encode the machine states in the [circpad_machine_spec_t structure](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L655). Each machine state is described by a [circpad_state_t structure](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L273) and each such structure specifies the style and amount of padding to be sent, as well as the possible state transitions. 

The function `circpad_machine_states_init()` must be used for allocation and initialization `circpad_machine_spec_t.states` array field before states and state transitions can get defined, as some of the state object has non-zero initial values.

### 3.2. Padding Machine State Transitions

As described above, padding machines can have multiple states so that they can support different forms of padding. Machines can transition between states based on certain events that occur either on the circuit-level or on the machine-level.

State transitions are specified using the [next_state field](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding.h#L381) of the `circpad_state_t` structure. As a simplistic example, to transition from state `A` to state `B` when event `E` occurs, you should implement the following code: `A.next_state[E] = B`. For more implementation details, please see the section XXXHSMachines below.

#### 3.2.1. State transition events

Here we will go through [the various events](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding.h#L30) that can be used to transition between states:

* Circuit-level events
  * `CIRCPAD_EVENT_NONPADDING_RECV`: A non-padding cell is received
  * `CIRCPAD_EVENT_NONPADDING_SENT`: A non-adding cell is sent
  * `CIRCPAD_EVENT_PADDING_SENT`: A padding cell is sent
  * `CIRCPAD_EVENT_PADDING_RECV`: A padding cell is received
* Machine-level events
  * `CIRCPAD_EVENT_INFINITY`: Tried to schedule padding using the ''infinity bin''.
  * `CIRCPAD_EVENT_BINS_EMPTY`: All histogram bins are empty (out of tokens)
  * `CIRCPAD_EVENT_LENGTH_COUNT`: State has used all its padding capacity (see `length_dist` below)

### 3.3. How padding gets specified in a machine state

Each state of a padding machine specifies either:
  * A padding histogram describing inter-arrival cell delays; OR
  * A parameterized delay probability distribution for inter-arrival cell delays

Either mechanism specifies essentially the *minimum inter-arrival time* distrubtion. If non-padding traffic does not get transmitted from this endpoint before the delay value sampled from this distribution expires, a padding packet is sent.

Picking between histograms and probability distributions can be subtle. A rule of thumb is that probability distributions are easy to specify and consume very little memory, but might not be able to describe certain types of intricate padding logic. Histograms on the other hand can support precise packet-count oriented or multimodal delay schemes, and can use token removal logic to reduce overhead and shape the total padding+non-padding inter-packet delay distribution towards an overall target distribution.

We suggest you start with a probability distribution if possible, and if it doesn't suit your needs you move to a histogram-based approach.

#### 3.3.1. Padding probability distributions

The easiest, most compact way to schedule padding using a machine state is to use a probability distribution that specifies the possible delays. That can be done [using the circpad_state_t fields](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L339) `iat_dist`, `dist_max_sample_usec` and `dist_added_shift_usec`.

The Tor circuit padding framework [supports multiple types](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L214) of probability distributions, and the developer should use the [circpad_distribution_t structure](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L240) to specify them as well as the required parameters.

#### 3.3.2. Padding Histograms

A more advanced way to schedule padding is to use a ''padding histogram''. The main advantages of a histrogram are that it allows you to specify distributions that are not easily parameterized in closed form, or require specific packet counts at particular time intervals. Histograms also allow you to make use of an optional traffic minimization and shaping optimization called *token removal*, which is cental to the original [Adaptive Padding](https://www.freehaven.net/anonbib/cache/ShWa-Timing06.pdf) concept.

If a histogram is used by a state (as opposed to a fixed parameterized
distribution), then the developer must use the [histogram related fields](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L285) of the `circpad_state_t` structure.

The width of a histogram specifies the range of inter-packet delay times, whereas its height specifies the amount of tokens in each bin. To sample a padding delay from a histogram, we first randomly pick a bin (weighted by the amount of tokens in each bin) and then sample a delay from within that bin by picking a uniformly random delay using the width of the bin as the range.

Each histogram also has an ''infinity bin'' as its final bin, and if that's chosen then we don't schedule any padding (i.e. we schedule padding with infinite delay). If the developer does not want infinite delay, then they can just not give any tokens to the ''infinity bin''.

If a token removal strategy is specified (via the `circpad_state_t.token_removal` field), each time padding is sent using a histogram, the padding machine should remove a token from the appropriate histogram bin whenever this endpoint sends *either a padding packet or a non-padding packet*. The different removal strategies govern what to do when the bin corresponding to the current inter-packet delay is empty.

Token removal is optional. It is useful if you want to do things like specify a burst should be at least N packets long, and you only want to add padding packets if there are not enough non-padding packets. The cost of doing token removal is additional memory allocations for making per-circuit copies of your histogram that can be modified.

### 3.4. Specifying precise padding amounts

Padding machines should be able to specify the exact amount of padding they send. For histogram-based machines this can be done using a specific amount of tokens, but another (and perhaps easier) way to do this, is to use the [length_dist field](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.h#L362) of the `circpad_state_t` structure.

The `length_dist` field is basically a probability distribution similar to the padding probability distributions we talked about in section XXX, which applies to a specific machine state and specifies the amount of padding we are willing to send during that state. This value gets sampled when we transition to that state (TODO document this in the code).

### 3.5. Specifying overhead limits

XXX: Describe global and per-machine overhead rate limiting

## 4. Evaluating new machines

One of the goals of the circuit padding framework is to provide improved evaluation and scientific reproducibility for lower cost.

So far, whenever evaluation cost has been a barrier, each research group has developed their own ad-hoc packet-level simulators of various padding mechanisms for evaluating website fingerprinting attacks and defenses. The process typically involves doing a crawl of Alexa top sites over Tor, and recording the Tor cell count and timing information for each page in the trace. These traces are then fed to simulations of defenses, which output modified trace files.

Because no standardized simulation and evaluation mechanism exists, it is often hard to tell if independent implementations of various attacks and defenses are in fact true-to-form or even properly calibrated for direct comparison, and discrepencies in results across the literature strongly suggests this is not always so.

XXX: Which order should we put these in? Maybe simulation, then emulation, then live makes more sense? That's the order things will get done in practice, probably...
[i think it's ok as this is]

### 4.1. Live Network Testing

Live network testing is the gold standard for verifying that any attack or defense is behaving as expected, to minimize the influence of simplifying assumptions.

However, it is not ethical, or even possible, to run high-resolution traffic analysis attacks on the entire Tor network. But, it is both ethical and posssible to run small scale experiments that target only your own clients, who will only use your own Tor relays that support your new padding machines.

We provide the `MiddleNodes` torrc directive to enable this, which will allow you to specify the fingerprints and/or IP netmasks of relays to be used in the second hop position. Options to restrict other hops also exist, if your padding system is padding to a different hop. The `HSLayer2Nodes` option overrides the `MiddleNodes` option for onion service circuits, if both are set. (The [vanguards addon](https://github.com/mikeperry-tor/vanguards/README_TECHNICAL.md) will set `HSLayer2Nodes`.)

When you run your own clients, and use MiddleNodes to restrict your clients to use your relays, you can perform live network evaluations of a defense applied to whatever traffic crawl or activity your clients do.

### 4.2. Chutney

The Tor Project provides a tool called [Chutney](https://github.com/torproject/chutney/) which makes it very easy to setup private Tor networks. While getting it work for the first time might take you some time of doc reading, the final result is well worth it for the following reasons:
- You control all the relays and hence you have greater control and debugging capabilities.
- You control all the relays and hence you can toggle padding support on/off at will.
- You don't need to be cautious about overhead or damaging the real Tor network during testing.
- You don't even need to be online; you can do all your testing offline over localhost.

A final word of warning here is that since Chutney runs over localhost, the packet latencies and delays are completely different from the real Tor network, so if your padding machines rely on real network timings you will get different results on Chutney. You can work around this by using a different set of delays if Chutney is used, or by moving your padding machines to the real network when you want to do latency-related testing.

### 4.3. Shadow

Shadow is an environment for running entire Tor network simulations, similar to Chutney, but it is meant to be both more memory efficient, as well as provide an accurate Tor network topology and latency model.

XXX: Link to Rob's docs + models

XXX: Do we want to mention netmirage here, too? It is supposed to be compatible with shadow models soon.

### 4.4. Pure Simulation

When doing initial tuning of padding machines, especially in adversarial settings, variations of a padding machine defense may need to be applied to network activity hundreds or even millions of times. The wall-clock time required to do this kind of tuning using live testing or even Shadow network emulation may often be prohibitive.

However, because the circuit padding system is event-driven and is otherwise only loosly coupled with Tor, it should be possible to extract circuitpadding.[ch], circuitpadding_machines.[ch], and a handful of headers from Tor, and use packet and event traces from Tor to call in to the circuitpadding.c event callbacks, based only on recorded traces.

In this way, a live crawl of the Alexa top sites could be performed once, to produce a standard "undefended" corpus. Padding machines can be then quickly evaluated on these simulated traces in a standardized way. As of this writing, this has not yet been done. 

See Section XXX for specific tor implementation details and pointers on how to do this successfully.

## 5. Example Padding Machines

### 5.1. Deployed Onion Service Circuit Setup Machines

Tor currently has two padding machines enabled by default, which aim to hide certain features of the client-side onion service circuit setup protocol. For more details on their precise goal and function, please see [proposal 302](https://github.com/torproject/torspec/blob/master/proposals/302-padding-machines-for-onion-clients.txt) . In this section we will go over the code of those machines to clarify some of the engineering parts:

#### 5.1.1. Overview

The codebase of proposal 302 can be found in [circuitpadding_machines.c](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding_machines.c) and specifies four padding machines:
- The [client-side introduction](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L60) circuit machine.
- The [relay-side introduction](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L146) circuit machine.
- The [client-side rendezvous](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L257) circuit machine
- The [relay-side rendezvous](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L374) circuit machine.

Each of those machines has its own setup function, and [they are all initialized](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding.c#L2718) by the circuit padding framework. To understand more about them, please carefully read the individual setup function for each machine which are fairly well documented. Each function goes through the following steps:
- Machine initialization
  - Give it a [name](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L70)
  - Specify [which hop](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L73) the padding should go to
  - Specify whether it should be [client-side](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L75) or relay-side.
- Specify for [which types of circuits](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L78) the machine should apply
- Specify whether the circuits should be [kept alive](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding_machines.c#L112) until the machine finishes padding.
- Sets [padding limits](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L116) to avoid too much overhead in case of bugs or errors.
- Setup [machine states](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L120)
   - Specify [state transitions](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L123).
- Finally [register the machine](https://github.com/torproject/tor/blob/35e978da61efa04af9a5ab2399dff863bc6fb20a/src/core/or/circuitpadding_machines.c#L137) to the global machine list

### 5.2. Other machines (?)

XXX: Ask other researchers to help fill in subsections for these machines

   - WTF-PAD [all examples below can be ditched for MVP. perhaps just a small overview]
   - APE
   - REB (from
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - No-Delay RBB (Bases on
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - Multiple machines with matching conditions
   - Reimplementation of netflow padding


## 6. Advanced Topics and Implementation Details

This section is meant for developers who need to add additional events, conditions, or other features to the circuit padding framework.

### 6.1. Memory Allocation and Pointer Lifespans

If the existing circuit padding features are sufficient for your needs, then you do not need to worry about memory management or pointer lifespans. The circuit padding framework should take care of this for you automatically.

However, if you need to add new padding machine features to support your padding machines, then it will be helpful to understand how circuits correspond to the global machine definitions, and how mutable padding machine memory is managed.

#### 6.1.1. Circuits and Padding Machines

In Tor, the [circuit_t structure](https://github.com/torproject/tor/blob/master/src/core/or/circuit_st.h) is the superclass structure for circuit-related state that is used on both clients and relays. On clients, the actual datatype of the object pointed to by `circuit_t *` is the subclass structure [origin_circuit_t](https://github.com/torproject/tor/blob/master/src/core/or/origin_circuit_st.h). The macros `CIRCUIT_IS_ORIGIN()` and `TO_ORIGIN_CIRCUIT()` are used to determine if a circuit is a client-side (origin) circuit and to cast the pointer safely to `origin_circuit_t *`.

Because circuit padding machines can be present at both clients and relays, the circuit padding fields are stored in the `circuit_t *` superclass structure. Notice that there are actually two sets of circuit padding fields: a `const circpad_machine_spec_t *` array, and a `circpad_machine_runtime_t *` array. Each of these arrays holds at most two elements, as there can be at most two padding machines on each circuit.

The `const circpad_machine_spec_t *` points to a globally allocated machine specification, as described in [section 1.1.XXX](#1.1HeaderWalkthrough). These machine specifications are allocated and set up during Tor program startup, in `circpad_machines_init()` in [circuitpadding.c](https://github.com/torproject/tor/blob/master/src/core/or/circuitpadding.c). Because the machine specification object is shared by all circuits, it must not be modified or freed until program exit (by `circpad_machines_free()`). The `const` qualifier  should enforce this at compile time.

The `circpad_machine_runtime_t *` arrary member points to the mutable runtime information for machine specification at that same array index. This runtime structure keeps track of the current machine state, packet counts, and other information that must be updated as the machine operates. When a padding machine is successfully negotiated (see Section 2.XXX), `circpad_setup_machine_on_circ()` allocates the associated runtime information.

#### 6.1.2. Histogram Management

If a `circpad_state_t` of a machine specifies a `token_removal` strategy other than `CIRCPAD_TOKEN_REMOVAL_NONE` (see section XXX), then every time there is a state transition into this state, `circpad_machine_setup_tokens()` will copy the read-only `circpad_state_t.historam` array into a mutable version at `circpad_machine_runtime_t.histogram`. This mutable copy is used to decrement the histogram bin acounts as packets are sent, as per the specified token removal strategy.

When the state machine transitions out of this state, the mutable histogram copy is freed
by this same `circpad_machine_setup_tokens()` function.

#### 6.1.3. Deallocation and Shutdown

XXX: machine_info can be missing but spec can remain (active vs inactive machines)

## 7. Future Features and Optimizations

While implementing the circuit padding framework, our goal was to deploy a system that obscured client-side onion service circuit setup and supported deployment of WTF-PAD and/or APE. Along the way, we noticed several features that might prove useful to others, but were not essential to implement immediately. We do not have immediate plans to implement these ideas, but we would gladly accept patches that do so.

### 7.1. Load Balancing and Flow Control

Fortunately, non-Exit bandwidth is already plentiful and exceeds the Exit capacity, and we anticipate that if we inform our relay operator community of the need for non-Exit bandwidth to satisfy padding overhead requirements, they will be able to provide that with relative ease.

Unfortunately, padding machines that have large quantities of overhead will require changes to our load balancing system to account for this overhead. The necessary changes are documented in [Proposal 265](https://gitweb.torproject.org/torspec.git/tree/proposals/265-load-balancing-with-overhead.txt). 

Additionally, padding cells are not currently subjected to flow control. For high amounts of padding, we may want to change this. See ticket XXX for details.

### 7.2. Circuitmux Optimizations

As of this writing (and Tor 0.4.1-stable), the cell event callbacks come from post-decryption points in the cell processing codepath, and from the pre-queue points in the cell send codepath. This means that our cell events will not reflect the actual time when packets are read or sent on the wire. This is much worse in the send direction, as the circuitmux queue, channel outbuf, and kernel TCP buffer will impose significant additional delay between when we currently report that a packet was sent, and when it actually hits the wire.

[Ticket 29494](https://trac.torproject.org/projects/tor/ticket/29494) has a more detailed description of this problem, and an experimental branch that changes the cell event callback locations to be from circuitmux post-queue, which with KIST, should be an accurate reflection of when they are actually sent on the wire.

If your padding machine and problem space depends on very accurate notions of relay-side packet timing, please try that branch and let us know on the ticket if you need any further assistance fixing it up.

Additionally, with that change, it will be possible to provide further overhead reducing optimizations by letting machines specify flags to indicate that padding should not be sent if there are any cells pending in the cell queue, for doing things like extending cell bursts more accurately and with less overhead.

### 7.3. Machine Negotiation

  - More machine conditions
     - Exit Policy-based Stream Conditions
       - https://trac.torproject.org/projects/tor/ticket/29083
     - Probability to apply machine/Cointoss condition
       - https://trac.torproject.org/projects/tor/ticket/30092
     - Probabilities/delay distributions for launching new padding circuit(s)
       - XXX: File ticket (or is this #30092 also?)

   - Better Negotiation
     - Always send negotiation cell on all circuits
       - https://trac.torproject.org/projects/tor/ticket/30172
     - Better shutdown handling
       - https://trac.torproject.org/projects/tor/ticket/30992
     - Preference-ordered menu
       - https://trac.torproject.org/projects/tor/ticket/30348

### 7.4. Probabalstic State Transitions

Right now, the state machine transitions are fully deterministic. However, one could imagine a state machine that uses probabalistic transitions between states to simulate a random walk or Hidden Markov Model traversal across several pages.

For this, the next_state array would have to become two dimensional, so that an array of next state and probability pairs could be provided. If you need this feature, please see ticket XXX.

### 7.5. Improved simulation mechanisms

  - XXX: Ticket for event tracing log messages (and padanalys logs)
  - XXX: Ticket for refactoring circuitpadding.c into easy-to-stub helpers
    - Reuse the stupid "split file becuase too many lines" ticket..
      - Our pedantic style instincts where...useful? gasp. Maybe just make new ticket ;)


