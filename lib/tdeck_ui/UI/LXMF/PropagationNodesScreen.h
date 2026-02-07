// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_PROPAGATIONNODESSCREEN_H
#define UI_LXMF_PROPAGATIONNODESSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <functional>
#include "Bytes.h"

namespace LXMF {
	class PropagationNodeManager;
}

namespace UI {
namespace LXMF {

/**
 * Propagation Nodes Screen
 *
 * Shows a list of discovered LXMF propagation nodes with selection:
 * - Node name and hash
 * - Hop count / reachability
 * - Selection radio buttons
 * - Auto-select option
 *
 * Layout:
 * +-------------------------------------+
 * | <- Prop Nodes              [Sync]  | 32px header
 * +-------------------------------------+
 * | ( ) Auto-select best node          |
 * +-------------------------------------+
 * | (*) NodeName1           2 hops     |
 * |     abc123...                      |
 * +-------------------------------------+
 * | ( ) NodeName2           3 hops     | 168px scrollable
 * |     def456...           disabled   |
 * +-------------------------------------+
 */
class PropagationNodesScreen {
public:
    /**
     * Node item data for display
     */
    struct NodeItem {
        RNS::Bytes node_hash;
        String name;
        String hash_display;
        uint8_t hops;
        bool enabled;
        bool is_selected;
    };

    /**
     * Callback types
     */
    using NodeSelectedCallback = std::function<void(const RNS::Bytes& node_hash)>;
    using BackCallback = std::function<void()>;
    using SyncCallback = std::function<void()>;
    using AutoSelectChangedCallback = std::function<void(bool enabled)>;

    /**
     * Create propagation nodes screen
     * @param parent Parent LVGL object (usually lv_scr_act())
     */
    PropagationNodesScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~PropagationNodesScreen();

    /**
     * Load nodes from PropagationNodeManager
     * @param manager The propagation node manager
     * @param selected_hash Currently selected node hash (empty for auto-select)
     * @param auto_select_enabled Whether auto-select is enabled
     */
    void load_nodes(::LXMF::PropagationNodeManager& manager,
                   const RNS::Bytes& selected_hash,
                   bool auto_select_enabled);

    /**
     * Refresh the display
     */
    void refresh();

    /**
     * Set callback for node selection
     * @param callback Function to call when a node is selected
     */
    void set_node_selected_callback(NodeSelectedCallback callback);

    /**
     * Set callback for back button
     * @param callback Function to call when back is pressed
     */
    void set_back_callback(BackCallback callback);

    /**
     * Set callback for sync button
     * @param callback Function to call when sync is pressed
     */
    void set_sync_callback(SyncCallback callback);

    /**
     * Set callback for auto-select toggle
     * @param callback Function to call when auto-select is changed
     */
    void set_auto_select_changed_callback(AutoSelectChangedCallback callback);

    /**
     * Show the screen
     */
    void show();

    /**
     * Hide the screen
     */
    void hide();

    /**
     * Get the root LVGL object
     * @return Root object
     */
    lv_obj_t* get_object();

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _list;
    lv_obj_t* _btn_back;
    lv_obj_t* _btn_sync;
    lv_obj_t* _auto_select_row;
    lv_obj_t* _auto_select_checkbox;
    lv_obj_t* _empty_label;

    std::vector<NodeItem> _nodes;
    RNS::Bytes _selected_hash;
    bool _auto_select_enabled;

    // Callbacks
    NodeSelectedCallback _node_selected_callback;
    BackCallback _back_callback;
    SyncCallback _sync_callback;
    AutoSelectChangedCallback _auto_select_changed_callback;

    // UI construction
    void create_header();
    void create_auto_select_row();
    void create_list();
    void create_node_item(const NodeItem& item, size_t index);
    void show_empty_state();
    void update_selection_ui();

    // Event handlers
    static void on_node_clicked(lv_event_t* event);
    static void on_back_clicked(lv_event_t* event);
    static void on_sync_clicked(lv_event_t* event);
    static void on_auto_select_changed(lv_event_t* event);

    // Utility
    static String truncate_hash(const RNS::Bytes& hash);
    static String format_hops(uint8_t hops);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_PROPAGATIONNODESSCREEN_H
