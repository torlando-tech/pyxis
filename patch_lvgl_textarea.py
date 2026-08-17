"""Make the transient LVGL textarea constructor fail closed on allocation failure.

LVGL 8.3.11 dereferences a null outer object after lv_obj_class_create_obj()
and dereferences a null child label inside the textarea constructor. Pyxis form
fields are remote input, so editor creation must return NULL to the checked
application path instead of crashing under memory pressure.
"""

import argparse
import os
import sys

pio_import = globals().get("Import")
if pio_import is not None:
    pio_import("env")
    env = globals()["env"]
    sys.path.insert(0, env.get("PROJECT_DIR", "."))
    from _build_helpers import env_libdeps_dir

    def lvgl_path(*parts):
        return env_libdeps_dir(env, "lvgl", *parts)
else:
    parser = argparse.ArgumentParser(
        description="Apply only Pyxis' deterministic LVGL source hardening patch")
    parser.add_argument("--lvgl-root", required=True)
    args = parser.parse_args()
    standalone_lvgl_root = os.path.abspath(args.lvgl_root)

    def lvgl_path(*parts):
        return os.path.join(standalone_lvgl_root, *parts)


path = lvgl_path("src", "widgets", "lv_textarea.c")
if not os.path.exists(path):
    raise RuntimeError(f"LVGL textarea source not found: {path}")

with open(path, "r", encoding="utf-8") as source:
    content = source.read()

outer_old = """    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
"""
outer_new = """    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    if(obj == NULL) return NULL;  /* patched by Pyxis: allocation is fallible */
    lv_obj_class_init_obj(obj);
    lv_textarea_t * ta = (lv_textarea_t *)obj;
    if(ta->label == NULL) {
        lv_obj_del(obj);
        return NULL;
    }
    return obj;
"""

label_old = """    ta->label = lv_label_create(obj);
    lv_obj_set_width(ta->label, lv_pct(100));
"""
label_intermediate = """    ta->label = lv_label_create(obj);
    if(ta->label == NULL) return;  /* patched by Pyxis: checked by creator */
    lv_obj_set_width(ta->label, lv_pct(100));
"""
label_new = """    ta->label = lv_obj_class_create_obj(&lv_label_class, obj);
    if(ta->label == NULL) return;  /* patched by Pyxis: allocation is fallible */
    lv_obj_class_init_obj(ta->label);
    lv_obj_set_width(ta->label, lv_pct(100));
"""

password_old = """    lv_label_ins_text(ta->label, ta->cursor.pos, letter_buf); /*Insert the character*/
    lv_textarea_clear_selection(obj); /*Clear selection*/

    if(ta->pwd_mode) {
        /*+2: the new char + \\0*/
        size_t realloc_size = strlen(ta->pwd_tmp) + strlen(letter_buf) + 1;
        ta->pwd_tmp = lv_mem_realloc(ta->pwd_tmp, realloc_size);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;

        _lv_txt_ins(ta->pwd_tmp, ta->cursor.pos, (const char *)letter_buf);
"""
password_intermediate = """    char * pwd_resized = NULL;
    if(ta->pwd_mode) {
        /* Reserve before changing the visible label. Never overwrite the only
         * plaintext pointer with a failed realloc result. Patched by Pyxis. */
        size_t realloc_size = strlen(ta->pwd_tmp) + strlen(letter_buf) + 1;
        pwd_resized = lv_mem_realloc(ta->pwd_tmp, realloc_size);
        LV_ASSERT_MALLOC(pwd_resized);
        if(pwd_resized == NULL) return;
    }

    lv_label_ins_text(ta->label, ta->cursor.pos, letter_buf); /*Insert the character*/
    lv_textarea_clear_selection(obj); /*Clear selection*/

    if(ta->pwd_mode) {
        ta->pwd_tmp = pwd_resized;
        _lv_txt_ins(ta->pwd_tmp, ta->cursor.pos, (const char *)letter_buf);
"""
password_pointer_new = password_intermediate.replace(
    "        if(pwd_resized == NULL) return;\n    }\n\n    lv_label_ins_text",
    "        if(pwd_resized == NULL) return;\n        ta->pwd_tmp = pwd_resized;\n    }\n\n    lv_label_ins_text",
).replace("        ta->pwd_tmp = pwd_resized;\n        _lv_txt_ins", "        _lv_txt_ins")
password_new = password_pointer_new.replace(
    "    lv_label_ins_text(ta->label, ta->cursor.pos, letter_buf); /*Insert the character*/\n",
    "    size_t label_len_before = strlen(lv_label_get_text(ta->label));\n"
    "    lv_label_ins_text(ta->label, ta->cursor.pos, letter_buf); /*Insert the character*/\n"
    "    if(strlen(lv_label_get_text(ta->label)) != label_len_before + strlen(letter_buf)) return;\n",
)

bulk_old = """    /*Insert the text*/
    lv_label_ins_text(ta->label, ta->cursor.pos, txt);
    lv_textarea_clear_selection(obj);

    if(ta->pwd_mode) {
        size_t realloc_size = strlen(ta->pwd_tmp) + strlen(txt) + 1;
        ta->pwd_tmp = lv_mem_realloc(ta->pwd_tmp, realloc_size);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;

        _lv_txt_ins(ta->pwd_tmp, ta->cursor.pos, txt);
"""
bulk_intermediate = """    char * pwd_bulk_resized = NULL;
    if(ta->pwd_mode) {
        size_t realloc_size = strlen(ta->pwd_tmp) + strlen(txt) + 1;
        pwd_bulk_resized = lv_mem_realloc(ta->pwd_tmp, realloc_size);
        LV_ASSERT_MALLOC(pwd_bulk_resized);
        if(pwd_bulk_resized == NULL) return;
    }

    /*Insert the text only after password storage is reserved. Patched by Pyxis.*/
    lv_label_ins_text(ta->label, ta->cursor.pos, txt);
    lv_textarea_clear_selection(obj);

    if(ta->pwd_mode) {
        ta->pwd_tmp = pwd_bulk_resized;
        _lv_txt_ins(ta->pwd_tmp, ta->cursor.pos, txt);
"""
bulk_pointer_new = bulk_intermediate.replace(
    "        if(pwd_bulk_resized == NULL) return;\n    }\n\n    /*Insert",
    "        if(pwd_bulk_resized == NULL) return;\n        ta->pwd_tmp = pwd_bulk_resized;\n    }\n\n    /*Insert",
).replace("        ta->pwd_tmp = pwd_bulk_resized;\n        _lv_txt_ins", "        _lv_txt_ins")
bulk_new = bulk_pointer_new.replace(
    "    lv_label_ins_text(ta->label, ta->cursor.pos, txt);\n",
    "    size_t label_len_before = strlen(lv_label_get_text(ta->label));\n"
    "    lv_label_ins_text(ta->label, ta->cursor.pos, txt);\n"
    "    if(strlen(lv_label_get_text(ta->label)) != label_len_before + strlen(txt)) return;\n",
)

delete_old = """        ta->pwd_tmp = lv_mem_realloc(ta->pwd_tmp, strlen(ta->pwd_tmp) + 1);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;
"""
delete_new = """        char * pwd_shrunk = lv_mem_realloc(ta->pwd_tmp, strlen(ta->pwd_tmp) + 1);
        LV_ASSERT_MALLOC(pwd_shrunk);
        if(pwd_shrunk != NULL) ta->pwd_tmp = pwd_shrunk; /* old storage remains valid on failure */
"""

set_old = """        ta->pwd_tmp = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;
        strcpy(ta->pwd_tmp, txt);
"""
set_new = """        char * pwd_set_resized = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(pwd_set_resized);
        if(pwd_set_resized == NULL) return; /* preserve the existing plaintext for secure cleanup */
        ta->pwd_tmp = pwd_set_resized;
        strcpy(ta->pwd_tmp, txt);
"""

set_prefix_old = """    lv_textarea_t * ta = (lv_textarea_t *)obj;

    /*Clear the existing selection*/
    lv_textarea_clear_selection(obj);
"""
set_prefix_intermediate = """    lv_textarea_t * ta = (lv_textarea_t *)obj;
    const bool pwd_set_characterwise = lv_textarea_get_accepted_chars(obj) || lv_textarea_get_max_length(obj);
    if(ta->pwd_mode) {
        /* Reserve password storage before mutating the label. Patched by Pyxis. */
        char * pwd_set_resized = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(pwd_set_resized);
        if(pwd_set_resized == NULL) return;
        ta->pwd_tmp = pwd_set_resized;
    }

    /*Clear the existing selection*/
    lv_textarea_clear_selection(obj);
"""
set_prefix_new = """    lv_textarea_t * ta = (lv_textarea_t *)obj;
    const uint32_t set_max_length = lv_textarea_get_max_length(obj);
    const bool pwd_set_characterwise = lv_textarea_get_accepted_chars(obj) ||
        (set_max_length && _lv_txt_get_encoded_length(txt) > set_max_length);
    if(ta->pwd_mode) {
        /* Reserve password storage before mutating the label. Patched by Pyxis. */
        char * pwd_set_resized = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(pwd_set_resized);
        if(pwd_set_resized == NULL) return;
        ta->pwd_tmp = pwd_set_resized;
    }

    /*Clear the existing selection*/
    lv_textarea_clear_selection(obj);
"""

set_characterwise_old = """    if(lv_textarea_get_accepted_chars(obj) || lv_textarea_get_max_length(obj)) {
        lv_label_set_text(ta->label, "");
        lv_textarea_set_cursor_pos(obj, LV_TEXTAREA_CURSOR_LAST);
"""
set_characterwise_new = """    if(pwd_set_characterwise) {
        lv_label_set_text(ta->label, "");
        if(lv_label_get_text(ta->label)[0] != '\\0') return;
        lv_textarea_set_cursor_pos(obj, LV_TEXTAREA_CURSOR_LAST);
"""

set_direct_old = """    else {
        lv_label_set_text(ta->label, txt);
        lv_textarea_set_cursor_pos(obj, LV_TEXTAREA_CURSOR_LAST);
    }

    /*If the textarea is empty, invalidate it to hide the placeholder*/
"""
set_direct_new = """    else {
        lv_label_set_text(ta->label, txt);
        if(strcmp(lv_label_get_text(ta->label), txt) != 0) return;
        lv_textarea_set_cursor_pos(obj, LV_TEXTAREA_CURSOR_LAST);
    }

    /*If the textarea is empty, invalidate it to hide the placeholder*/
"""
set_final_old = """    if(ta->pwd_mode) {
        ta->pwd_tmp = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;
        strcpy(ta->pwd_tmp, txt);

        /*Auto hide characters*/
        auto_hide_characters(obj);
    }
"""
set_final_intermediate = """    if(ta->pwd_mode) {
        char * pwd_set_resized = lv_mem_realloc(ta->pwd_tmp, strlen(txt) + 1);
        LV_ASSERT_MALLOC(pwd_set_resized);
        if(pwd_set_resized == NULL) return; /* preserve the existing plaintext for secure cleanup */
        ta->pwd_tmp = pwd_set_resized;
        strcpy(ta->pwd_tmp, txt);

        /*Auto hide characters*/
        auto_hide_characters(obj);
    }
"""
set_final_new = """    if(ta->pwd_mode) {
        if(!pwd_set_characterwise) strcpy(ta->pwd_tmp, txt);

        /*Auto hide characters*/
        auto_hide_characters(obj);
    }
"""

password_mode_old = """    ta->pwd_mode = en ? 1U : 0U;
    /*Pwd mode is now enabled*/
    if(en) {
        char * txt = lv_label_get_text(ta->label);
        size_t len = strlen(txt);

        ta->pwd_tmp = lv_mem_alloc(len + 1);
        LV_ASSERT_MALLOC(ta->pwd_tmp);
        if(ta->pwd_tmp == NULL) return;

        strcpy(ta->pwd_tmp, txt);

        pwd_char_hider(obj);

        lv_textarea_clear_selection(obj);
    }
    /*Pwd mode is now disabled*/
    else {
        lv_textarea_clear_selection(obj);
        lv_label_set_text(ta->label, ta->pwd_tmp);
        lv_mem_free(ta->pwd_tmp);
        ta->pwd_tmp = NULL;
    }
"""
password_mode_new = """    /* Commit password mode only after backing allocation succeeds. Patched by Pyxis. */
    if(en) {
        char * txt = lv_label_get_text(ta->label);
        size_t len = strlen(txt);
        char * pwd_mode_tmp = lv_mem_alloc(len + 1);
        LV_ASSERT_MALLOC(pwd_mode_tmp);
        if(pwd_mode_tmp == NULL) return;

        strcpy(pwd_mode_tmp, txt);
        ta->pwd_tmp = pwd_mode_tmp;
        ta->pwd_mode = 1U;
        pwd_char_hider(obj);
        lv_textarea_clear_selection(obj);
    }
    /*Pwd mode is now disabled*/
    else {
        ta->pwd_mode = 0U;
        lv_textarea_clear_selection(obj);
        lv_label_set_text(ta->label, ta->pwd_tmp);
        lv_mem_free(ta->pwd_tmp);
        ta->pwd_tmp = NULL;
    }
"""

changed = False
for old, new, label in (
    (outer_old, outer_new, "outer textarea allocation guard"),
    ((label_old, label_intermediate), label_new, "textarea child-label allocation guard"),
    ((password_old, password_intermediate, password_pointer_new), password_new, "password backing realloc guard"),
    ((bulk_old, bulk_intermediate, bulk_pointer_new), bulk_new, "password bulk realloc guard"),
    (delete_old, delete_new, "password shrink realloc guard"),
    ((set_prefix_old, set_prefix_intermediate), set_prefix_new, "password set preallocation guard"),
    (set_characterwise_old, set_characterwise_new, "constrained set clear guard"),
    (set_direct_old, set_direct_new, "transactional constrained replacement guard"),
    ((set_final_old, set_final_intermediate), set_final_new, "password set mutation ordering guard"),
    (password_mode_old, password_mode_new, "password mode allocation guard"),
):
    candidates = old if isinstance(old, tuple) else (old,)
    matched = next((candidate for candidate in candidates if candidate in content), None)
    if matched is not None:
        content = content.replace(matched, new, 1)
        changed = True
        print(f"PATCH: lv_textarea.c: {label}")
    elif new not in content:
        if label == "textarea child-label allocation guard" and \
           "if(lv_label_get_text(ta->label) == NULL)" in content:
            print(f"PATCH: lv_textarea.c: {label} (superseded by usable-label guard)")
            continue
        raise RuntimeError(f"LVGL textarea patch failed closed: missing {label} pattern")
    else:
        print(f"PATCH: lv_textarea.c: {label} (already applied)")

if changed:
    with open(path, "w", encoding="utf-8") as source:
        source.write(content)



def patch_lvgl_source(source_path, replacements):
    if not os.path.exists(source_path):
        raise RuntimeError(f"LVGL source not found: {source_path}")
    with open(source_path, "r", encoding="utf-8") as source:
        source_content = source.read()
    source_changed = False
    for old_source, new_source, description in replacements:
        candidates = old_source if isinstance(old_source, tuple) else (old_source,)
        matched_source = next((candidate for candidate in candidates if candidate in source_content), None)
        if matched_source is not None:
            source_content = source_content.replace(matched_source, new_source, 1)
            source_changed = True
            print(f"PATCH: {os.path.basename(source_path)}: {description}")
        elif new_source not in source_content:
            raise RuntimeError(f"LVGL patch failed closed: missing {description} pattern in {source_path}")
        else:
            print(f"PATCH: {os.path.basename(source_path)}: {description} (already applied)")
    if source_changed:
        with open(source_path, "w", encoding="utf-8") as source:
            source.write(source_content)


obj_class_path = lvgl_path("src", "core", "lv_obj_class.c")
obj_parent_old = """        if(parent->spec_attr == NULL) {
            lv_obj_allocate_spec_attr(parent);
        }

        if(parent->spec_attr->children == NULL) {
            parent->spec_attr->children = lv_mem_alloc(sizeof(lv_obj_t *));
            parent->spec_attr->children[0] = obj;
            parent->spec_attr->child_cnt = 1;
        }
        else {
            parent->spec_attr->child_cnt++;
            parent->spec_attr->children = lv_mem_realloc(parent->spec_attr->children,
                                                         sizeof(lv_obj_t *) * parent->spec_attr->child_cnt);
            parent->spec_attr->children[parent->spec_attr->child_cnt - 1] = obj;
        }
"""
obj_parent_new = """        if(parent->spec_attr == NULL) {
            lv_obj_allocate_spec_attr(parent);
            if(parent->spec_attr == NULL) {
                lv_mem_free(obj);
                return NULL;
            }
        }

        if(parent->spec_attr->children == NULL) {
            lv_obj_t ** children_initial = lv_mem_alloc(sizeof(lv_obj_t *));
            if(children_initial == NULL) {
                lv_mem_free(obj);
                return NULL;
            }
            parent->spec_attr->children = children_initial;
            parent->spec_attr->children[0] = obj;
            parent->spec_attr->child_cnt = 1;
        }
        else {
            uint32_t child_cnt_resized = parent->spec_attr->child_cnt + 1;
            lv_obj_t ** children_resized = lv_mem_realloc(parent->spec_attr->children,
                                                          sizeof(lv_obj_t *) * child_cnt_resized);
            if(children_resized == NULL) {
                lv_mem_free(obj);
                return NULL;
            }
            parent->spec_attr->children = children_resized;
            parent->spec_attr->child_cnt = child_cnt_resized;
            parent->spec_attr->children[child_cnt_resized - 1] = obj;
        }
"""
patch_lvgl_source(obj_class_path, ((obj_parent_old, obj_parent_new, "fallible child bookkeeping"),))

label_path = lvgl_path("src", "widgets", "lv_label.c")
label_create_old = """    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
"""
label_create_new = """    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    if(obj == NULL) return NULL;  /* patched by Pyxis: allocation is fallible */
    lv_obj_class_init_obj(obj);
    return obj;
"""
label_insert_old = """    label->text        = lv_mem_realloc(label->text, new_len + 1);
    LV_ASSERT_MALLOC(label->text);
    if(label->text == NULL) return;
"""
label_insert_new = """    char * label_resized = lv_mem_realloc(label->text, new_len + 1);
    LV_ASSERT_MALLOC(label_resized);
    if(label_resized == NULL) return;
    label->text = label_resized;
"""
label_self_old = """        label->text = lv_mem_realloc(label->text, strlen(label->text) + 1);
#endif

        LV_ASSERT_MALLOC(label->text);
        if(label->text == NULL) return;
"""
label_self_new = """        char * label_resized = lv_mem_realloc(label->text, strlen(label->text) + 1);
        LV_ASSERT_MALLOC(label_resized);
        if(label_resized == NULL) return;
        label->text = label_resized;
#endif
"""
label_set_old = """    else {
        /*Free the old text*/
        if(label->text != NULL && label->static_txt == 0) {
            lv_mem_free(label->text);
            label->text = NULL;
        }

#if LV_USE_ARABIC_PERSIAN_CHARS
        /*Get the size of the text and process it*/
        size_t len = _lv_txt_ap_calc_bytes_cnt(text);

        label->text = lv_mem_alloc(len);
        LV_ASSERT_MALLOC(label->text);
        if(label->text == NULL) return;

        _lv_txt_ap_proc(text, label->text);
#else
        /*Get the size of the text*/
        size_t len = strlen(text) + 1;

        /*Allocate space for the new text*/
        label->text = lv_mem_alloc(len);
        LV_ASSERT_MALLOC(label->text);
        if(label->text == NULL) return;
        strcpy(label->text, text);
#endif

        /*Now the text is dynamically allocated*/
        label->static_txt = 0;
    }
"""
label_set_new = """    else {
#if LV_USE_ARABIC_PERSIAN_CHARS
        size_t len = _lv_txt_ap_calc_bytes_cnt(text);
        char * label_new_text = lv_mem_alloc(len);
        LV_ASSERT_MALLOC(label_new_text);
        if(label_new_text == NULL) return;
        _lv_txt_ap_proc(text, label_new_text);
#else
        size_t len = strlen(text) + 1;
        char * label_new_text = lv_mem_alloc(len);
        LV_ASSERT_MALLOC(label_new_text);
        if(label_new_text == NULL) return;
        strcpy(label_new_text, text);
#endif
        /* Commit only after allocation and copying succeed. Patched by Pyxis. */
        if(label->text != NULL && label->static_txt == 0) lv_mem_free(label->text);
        label->text = label_new_text;
        label->static_txt = 0;
    }
"""
patch_lvgl_source(label_path, (
    (label_create_old, label_create_new, "outer label allocation guard"),
    (label_insert_old, label_insert_new, "label insertion realloc guard"),
    (label_self_old, label_self_new, "label self-realloc guard"),
    (label_set_old, label_set_new, "transactional label replacement"),
))

textarea_mask_old = """    char * txt_tmp = lv_mem_buf_get(enc_len * bullet_len + 1);

    uint32_t i;
"""
textarea_mask_intermediate = """    char * txt_tmp = lv_mem_buf_get(enc_len * bullet_len + 1);
    if(txt_tmp == NULL) return;  /* patched by Pyxis: masking allocation is fallible */

    uint32_t i;
"""
textarea_mask_v9 = """    char * txt_tmp = lv_mem_buf_get(enc_len * bullet_len + 1);
    if(txt_tmp == NULL) {
        volatile char * visible = (volatile char *)txt;
        for(size_t wipe_i = 0; wipe_i < strlen(txt); wipe_i++) visible[wipe_i] = 0;
        lv_label_set_text(ta->label, NULL);
        return;
    }

    uint32_t i;
"""
textarea_mask_new = """    char * txt_tmp = lv_mem_buf_get(enc_len * bullet_len + 1);
    if(txt_tmp == NULL) {
        volatile char * visible = (volatile char *)txt;
        size_t visible_len = strlen(txt);
        for(size_t wipe_i = 0; wipe_i < visible_len; wipe_i++) visible[wipe_i] = 0;
        lv_label_set_text(ta->label, NULL);
        return;
    }

    uint32_t i;
"""
textarea_mask_commit_old = """    lv_label_set_text(ta->label, txt_tmp);
    lv_mem_buf_release(txt_tmp);
"""
textarea_mask_commit_v9 = """    lv_label_set_text(ta->label, txt_tmp);
    if(strcmp(lv_label_get_text(ta->label), txt_tmp) != 0) {
        char * visible = lv_label_get_text(ta->label);
        if(visible != NULL) {
            volatile char * wipe = (volatile char *)visible;
            for(size_t wipe_i = 0; wipe_i < strlen(visible); wipe_i++) wipe[wipe_i] = 0;
            lv_label_set_text(ta->label, NULL);
        }
    }
    lv_mem_buf_release(txt_tmp);
"""
textarea_mask_commit_new = """    lv_label_set_text(ta->label, txt_tmp);
    if(strcmp(lv_label_get_text(ta->label), txt_tmp) != 0) {
        char * visible = lv_label_get_text(ta->label);
        if(visible != NULL) {
            volatile char * wipe = (volatile char *)visible;
            size_t masked_len = strlen(visible);
            for(size_t wipe_i = 0; wipe_i < masked_len; wipe_i++) wipe[wipe_i] = 0;
            lv_label_set_text(ta->label, NULL);
        }
    }
    lv_mem_buf_release(txt_tmp);
"""
patch_lvgl_source(path, (
    ((textarea_mask_old, textarea_mask_intermediate, textarea_mask_v9), textarea_mask_new, "password masking allocation guard"),
    ((textarea_mask_commit_old, textarea_mask_commit_v9), textarea_mask_commit_new, "password masking commit guard"),
))

textarea_label_init_old = """    ta->label = lv_obj_class_create_obj(&lv_label_class, obj);
    if(ta->label == NULL) return;  /* patched by Pyxis: allocation is fallible */
    lv_obj_class_init_obj(ta->label);
    lv_obj_set_width(ta->label, lv_pct(100));
    lv_label_set_text(ta->label, "");
    lv_obj_add_event_cb(ta->label, label_event_cb, LV_EVENT_ALL, NULL);
"""
textarea_label_init_new = """    ta->label = lv_obj_class_create_obj(&lv_label_class, obj);
    if(ta->label == NULL) return;  /* patched by Pyxis: allocation is fallible */
    lv_obj_class_init_obj(ta->label);
    if(lv_label_get_text(ta->label) == NULL) {
        lv_obj_del(ta->label);
        ta->label = NULL;
        return;
    }
    lv_obj_set_width(ta->label, lv_pct(100));
    lv_label_set_text(ta->label, "");
    if(lv_label_get_text(ta->label) == NULL ||
       lv_obj_add_event_cb(ta->label, label_event_cb, LV_EVENT_ALL, NULL) == NULL) {
        lv_obj_del(ta->label);
        ta->label = NULL;
        return;
    }
"""
patch_lvgl_source(path, ((textarea_label_init_old, textarea_label_init_new, "usable label initialization"),))

event_path = lvgl_path("src", "core", "lv_event.c")
event_add_old = """    lv_obj_allocate_spec_attr(obj);

    obj->spec_attr->event_dsc_cnt++;
    obj->spec_attr->event_dsc = lv_mem_realloc(obj->spec_attr->event_dsc,
                                               obj->spec_attr->event_dsc_cnt * sizeof(lv_event_dsc_t));
    LV_ASSERT_MALLOC(obj->spec_attr->event_dsc);

    obj->spec_attr->event_dsc[obj->spec_attr->event_dsc_cnt - 1].cb = event_cb;
    obj->spec_attr->event_dsc[obj->spec_attr->event_dsc_cnt - 1].filter = filter;
    obj->spec_attr->event_dsc[obj->spec_attr->event_dsc_cnt - 1].user_data = user_data;

    return &obj->spec_attr->event_dsc[obj->spec_attr->event_dsc_cnt - 1];
"""
event_add_new = """    lv_obj_allocate_spec_attr(obj);
    if(obj->spec_attr == NULL) return NULL;

    uint32_t event_dsc_cnt_new = obj->spec_attr->event_dsc_cnt + 1;
    lv_event_dsc_t * event_dsc_resized = lv_mem_realloc(obj->spec_attr->event_dsc,
                                                        event_dsc_cnt_new * sizeof(lv_event_dsc_t));
    LV_ASSERT_MALLOC(event_dsc_resized);
    if(event_dsc_resized == NULL) return NULL;
    obj->spec_attr->event_dsc = event_dsc_resized;
    obj->spec_attr->event_dsc_cnt = event_dsc_cnt_new;

    obj->spec_attr->event_dsc[event_dsc_cnt_new - 1].cb = event_cb;
    obj->spec_attr->event_dsc[event_dsc_cnt_new - 1].filter = filter;
    obj->spec_attr->event_dsc[event_dsc_cnt_new - 1].user_data = user_data;

    return &obj->spec_attr->event_dsc[event_dsc_cnt_new - 1];
"""
patch_lvgl_source(event_path, ((event_add_old, event_add_new, "transactional event registration"),))

group_path = lvgl_path("src", "core", "lv_group.c")
group_add_old = """    if(obj->spec_attr == NULL) lv_obj_allocate_spec_attr(obj);
    obj->spec_attr->group_p = group;

    lv_obj_t ** next = _lv_ll_ins_tail(&group->obj_ll);
    LV_ASSERT_MALLOC(next);
    if(next == NULL) return;
    *next = obj;
"""
group_add_new = """    if(obj->spec_attr == NULL) lv_obj_allocate_spec_attr(obj);
    if(obj->spec_attr == NULL) return;

    lv_obj_t ** next = _lv_ll_ins_tail(&group->obj_ll);
    LV_ASSERT_MALLOC(next);
    if(next == NULL) return;
    *next = obj;
    obj->spec_attr->group_p = group;
"""
patch_lvgl_source(group_path, ((group_add_old, group_add_new, "transactional group enrollment"),))
