                   Circuit Padding Developer Documentation
                              Mike Perry

XXX: Where should this doc live?
XXX: What format should it have? Plaintext or markdown?


0. Introduction

 - Adaptive Padding, WTF-PAD, APE

1. Overview

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
 - What to consider when creating a new machine
   - Placement in machine list (before/after)
   - Conditions of application
   - Timing needs
     - RTT estimates
     - Distribution accuracy
   - Overhead and optimizations
     - Token removal
     - Rate limiting options
     - Circuitmux throttling

2. Example Machines

   - Deployed HS Setup Machines
   - WTF-PAD
   - APE
   - REB (from
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - No-Delay RBB (Bases on
     https://www.researchgate.net/publication/329743510_UNDERSTANDING_FEATURE_DISCOVERY_IN_WEBSITE_FINGERPRINTING_ATTACKS)
   - Multiple machines with matching conditions

3. Potentially Desirable Future Features and Optimizations

   - Load balancing
     - Prop #265
   - Circuitmux optimizations
     - Improved event timing accuracy
     - Queue inspection
       - Queue conditions for sending padding
       - Queue conditions for token removal
   - New Machine Conditions
     - Exit Policy-based Stream Conditions
     - Probability to apply machine/Cointoss condition
     - Probabilities/delay distributions for launching new padding circuit(s)
   - Probabalistic State Transitions
   - Better Negotiation
     - Always send negotiation cell on all circuits
     - Better shutdown handling
     - Preference-ordered menu

4. Prototyping and evaluating new machines

   - Setting MiddleNodes
   - Using Shadow
   - Using NetMirage
   - Using Pure Simulation 
