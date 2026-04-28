# One Collar Design Context

*Product vision document. Origin story, functional overview, and technical
framing for the OneCollar platform.*

---

## Executive Summary

The patent-pending One Collar is designed to be the ultimate dog collar that integrates advanced technology to improve the relationship between dogs and their owners by managing behavior and location effectively. It aims to allow dogs to be safe and happy while enabling owners to spend more quality time with them.

**Origin and Purpose:** The collar was inspired by the founders' personal experience managing their two dogs' behavior and safety during a move, leading to the desire for a single, multifunctional collar to replace multiple devices.

**Location Tracking Features:** The collar supports precise outdoor tracking including virtual fences, exclusion zones, and ad hoc boundaries via GPS, WiFi, Bluetooth, and LoRa technology to allow flexible control over where the dog can roam.

**Behavior Monitoring:** It can identify various dog behaviors such as sitting, jumping, scratching, barking, and even bathroom habits, with the capability to customize behavior detection for individual dogs.

**Integration of Location and Behavior:** Combining location and behavior data enables fine-grained control over situations like preventing jumping in specific rooms or indoor barking, tailoring safety and management to owner needs.

Lastly the behaviors have been built on a platform, allowing for remotely deploying new behaviors to the collar, or for behaviors of other animals to be developed and deployed on different devices.

## Why One Collar Exists

When our son was born we had two dogs, Zoey and Elliot. Elliot: a super high energy Labrador Retriever, Zoey: a mix with herding instincts. As first time (human) parents we were balancing making sure the dogs got to know our baby while making sure any rambunctious play was safe. The reality of course is that meant the dogs spent less time with us as we got more comfortable being parents.

When our son Charlie was just 6 months old, we moved from Colorado to Baltimore for a job opportunity. Mom and Charlie flew comfortably to our new home, while Dad and Grandpa Bob got to drive two dogs across the entire country. On arrival in Reisterstown, that house had a fence that Zoey was constantly escaping.

> Note: Zoey is the evil genius and Elliot is the hapless muscle — she will literally take a ball (ball is life for Elliot) and tuck it next to a fence to get Elliot to excavate a cubic yard of dirt to facilitate her escape. Elliot stays behind and barks.

In order to deal with Zoey aka "Shawshank" we spent $1400 putting in a wireless fence that basically just outlined the physical fence and then made sure that Zoey had the wireless fence collar on at all times. At one point our dogs had six different collars: training collars, bark collars, inground fence collars, collars to open the dog door so they could come and go, and we experimented with GPS collars and activity collars. Of course half the time most of these collars were missing or had no charge when we needed them.

At the core of One Collar is a goal to integrate our dogs into our lives so that they can be healthy and happy and we can spend more time with them. We want to find that balance between managing their behavior and still letting them be dogs and we think that it should be OK to want to maximize that time together. Elliot will eat anything that's on the kitchen counter, that's just a fact. What if you could have a collar that would let Elliot hang out with us but also warn him not to go into the kitchen, or if we want him in the kitchen with us, be able to recognize when he's jumping or standing on his hind legs — which is never a good thing in the kitchen :)

This is the genesis story of One Collar.

## Functional Overview

One Collar is intended to be just that — One Collar that you will ever buy for your dog. Founded by dog lovers first, technologists second, One Collar is an extensible platform whose purpose is to allow you to live with your dog exactly the way you want. Do you have a 10 acre property that you want your dog to be able to roam? Our goal is to enable that so that you could simply mark the boundaries of your property on your phone and the collar will be able to determine when your dog is getting close to those boundaries. Maybe you discover a part of your property that you never want the dog to go on, so on that same map you identify an exclusion area. How about an ad hoc picnic in a place you want to let your dog roam? One Collar can make that happen.

### Key Concept: Location

Understanding where your pet is matters. We focus on two simple situations — indoors, and outdoors. We aspire to detailed tracking/mapping indoors, but at this stage are focused on "Indoors or not." We can do very high precision outdoor tracking (centimeters) with more expensive GPS + RTK, or less-precise (and less expensive) tracking with GPS on its own. Finally, we are experimenting with a technology called LoRa (low powered radio signals) that allows for very long range communications — 10+ Miles — in situations where WiFi and Bluetooth can't cover an area where the dogs should be able to go. Some of the functionality we support around location includes:

- Long-term virtual fence (at your home or locations you regularly visit)
- Ad hoc virtual fence (picnic, visiting a friend)
- Exclusion area (playground, garden, pond)
- Find My Dog — WiFi, Bluetooth, LoRa, other One Collar users (TBD)

### Key Concept: Behavior

Tracking dog behavior is One Collar's biggest differentiation. We can tell if your dog is sitting or laying quietly — or jumping up when/where they shouldn't. Behavior identification can be extended for brand new behaviors (my dog is army-crawling under a culvert) or trained for *your* dog (my dog poops… weird). As an extendable capability, this list is far from complete:

- Going to the bathroom
- Sleeping
- Getting up (any signs of hip dysplasia or arthritis?)
- Walking / running / jumping
- Scratching
- Barking

### Key Concept: Location + Behavior — AKA Blocation

While it may seem obvious that the intersection of location and behavior is something that would be useful to identify, we think it's a critical element to create fine-grained controls for creating an environment that is safe for your pet and family. Here are some examples:

- Jumping up in the kitchen (or nursery)
- Going to the bathroom indoors
- Barking indoors (… when the UPS guy isn't in front of the house)
- Sprinting at your toddler

The list literally goes on — the power is that you get to decide what combination of location and behaviors you need to manage.

## Technical Overview

Nerd time.

Patent pending, this collar consists of three distinct parts — the collar itself, an optional hub, and a machine learning model that interprets dog behavior.

### The Collar

The collar supports a range of communication mechanisms and sensors. For comms:

- WiFi
- Bluetooth
- LoRa
- Radio

For sensors: *[TBD — sensor list to be filled in. Authoritative sensor BOM lives in `02_onecollar_technical_architecture.md` §3 and `.claude/hardware.md`.]*

### The Hub

*[TBD — see `02_onecollar_technical_architecture.md` §9 for current open questions on hub scope.]*

### The Phone App

*[TBD — see `02_onecollar_technical_architecture.md` §10 for the Flutter target stack and current surfaces.]*

### The Cloud

*[TBD — see `02_onecollar_technical_architecture.md` §12 for Azure architecture.]*
