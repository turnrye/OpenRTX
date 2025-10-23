#!/bin/bash

EXPECTED_DOXYGEN_WARNING_COUNT=612

ACTUAL_DOXYGEN_WARNINGS=`doxygen 2>&1 | wc -l`

if [[ $ACTUAL_DOXYGEN_WARNINGS -ne $EXPECTED_DOXYGEN_WARNING_COUNT ]]; then
  echo "❌ Doxygen warnings count mismatch: expected $EXPECTED_DOXYGEN_WARNING_COUNT, got $ACTUAL_DOXYGEN_WARNINGS. Run Doxygen locally and see where you are missing adding documentation related to your changes."
  exit 1
fi
echo "✅ Doxygen warning count didn't change."
exit 0
