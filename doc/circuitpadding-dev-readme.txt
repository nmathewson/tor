                   Circuit Padding Developer Documentation
                              Mike Perry

XXX: Where should this doc live?
XXX: What format should it have? Plaintext or markdown?


0. Introduction

 - Adaptive Padding, WTF-PAD, APE, padding-spec.txt

1. System Overview

 - Header walkthrough
   - Events
     - Circuit events
     - Machine events
     - Internal events
   - State machine specification
     - Machine application conditions and flags
     - States
      - Histograms vs Parameterized Distributions
      - Specifying packet counts
      - Specifying state transitions
 - Circuit relationships
   - Runtime information vs spec
     - Allocation management
     - Pointer guarantees
   - Shutdown options
     - Deallocation points

2. What to consider when creating a new machine
   - Placement in machine list (before/after)
   - Conditions of application
   - Timing needs
     - RTT estimates
     - Distribution accuracy
   - Overhead and optimizations
     - Token removal
     - Rate limiting options
     - Circuitmux throttling
     - No Delay! There is No Anonymity Trilemma! Bandwidth Only!
       - https://freedom.cs.purdue.edu/anonymity/trilemma/index.html
       - Adding delays to real traffic will have better results
         for the same amount of overhead, but we will not accept
         such machines.

3. Example Machines

   - Deployed HS Setup Machines
   - WTF-PAD
   - APE
   - REB (from
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - No-Delay RBB (Bases on
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - Multiple machines with matching conditions

4. Potentially Desirable Future Features and Optimizations

   - Load balancing
     - Prop #265
       https://gitweb.torproject.org/torspec.git/tree/proposals/265-load-balancing-with-overhead.txt
   - Circuitmux optimizations
     - https://trac.torproject.org/projects/tor/ticket/29494
       - Improved event timing accuracy
       - Queue inspection
         - Queue conditions for sending padding
         - Queue conditions for token removal
   - New Machine Conditions
     - Exit Policy-based Stream Conditions
       - https://trac.torproject.org/projects/tor/ticket/29083
     - Probability to apply machine/Cointoss condition
       - https://trac.torproject.org/projects/tor/ticket/30092
     - Probabilities/delay distributions for launching new padding circuit(s)
       - XXX: File ticket (or is this #30092 also?)
   - Probabalistic State Transitions
     - XXX: File ticket
   - Better Negotiation
     - Always send negotiation cell on all circuits
       - https://trac.torproject.org/projects/tor/ticket/30172
     - Better shutdown handling
       - https://trac.torproject.org/projects/tor/ticket/30992
     - Preference-ordered menu
       - https://trac.torproject.org/projects/tor/ticket/30348

5. Prototyping and evaluating new machines

   - Setting MiddleNodes
   - Using Shadow
   - Using NetMirage
   - Using Pure Simulation 
