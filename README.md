# ut-control-17

## Repository summary

This repository provides **UT Control 17**, a C/CMake-based control-plane and configuration tooling library used to orchestrate automated test and validation workflows in the RDK ecosystem. It focuses on two main areas: a lightweight control plane for coordinating runs and exchanging messages with test clients (for example over WebSocket/HTTP), and a YAML/key–value configuration utility used to drive tests in a profile-driven, platform-independent way.

In the broader vDevice/RDK-E workflow, this project is typically used by engineering automation, CI, and device-validation tooling to load configuration profiles, coordinate test execution, and provide reusable utilities and example clients for interacting with a control endpoint.

## Contents at a glance

The repository includes the core library sources and headers, configuration/build scripts, and a substantial set of unit tests and test assets. Example clients demonstrate how to send payloads (for example JSON or YAML) to a running control-plane endpoint.
