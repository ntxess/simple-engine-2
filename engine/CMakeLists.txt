# Engine modules. Layering is enforced by se_check_link_graph() and the
# include scans (docs/decisions.md, ADR-004).

add_subdirectory(se_core)
add_subdirectory(se_os)
add_subdirectory(se_sim)
add_subdirectory(se_render_core)
