#pragma once

/**
 * @brief Marks an entity for destruction at the end of the current tick (rule R10,
 * docs/rules/gameplay-rules.md). Systems request; only the scene's Cleanup step destroys.
*/
struct DestroyRequest
{
};
