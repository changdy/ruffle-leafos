#!/bin/bash

# Generates the AVM2 report with the official stable report tool.
set -euo pipefail

cargo run --locked --package stub-report -- --avm2-report avm2_report.json
