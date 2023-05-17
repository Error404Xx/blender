/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Grid-view showing all assets according to the giving shelf-type and settings.
 */

#include "AS_asset_library.hh"

#include "BKE_screen.h"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_asset_handle.h"
#include "ED_asset_list.h"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.h"

#include "UI_grid_view.hh"
#include "UI_interface.h"
#include "UI_interface.hh"
#include "UI_view2d.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "WM_api.h"

#include "asset_shelf.hh"

namespace blender::ed::asset::shelf {

class AssetView : public ui::AbstractGridView {
  const AssetLibraryReference library_ref_;
  const AssetShelfSettings &shelf_settings_;
  std::optional<asset_system::AssetCatalogFilter> catalog_filter_ = std::nullopt;
  /* XXX Temporary: Only for #asset_poll__() callback. Should use traits instead. */
  bContext &evil_C_;

  friend class AssetDragController;

 public:
  AssetView(const AssetLibraryReference &library_ref,
            const AssetShelfSettings &shelf_settings,
            bContext &evil_C);

  void build_items() override;

  void set_catalog_filter(const std::optional<asset_system::AssetCatalogFilter> &catalog_filter);
};

class AssetViewItem : public ui::PreviewGridItem {
  AssetHandle asset_;
  bool allow_asset_drag_ = true;

 public:
  AssetViewItem(const AssetHandle &asset,
                StringRef identifier,
                StringRef label,
                int preview_icon_id);

  void disable_asset_drag();
  void build_grid_tile(uiLayout &layout) const override;
  void build_context_menu(bContext &C, uiLayout &column) const override;

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override;
};

class AssetDragController : public ui::AbstractViewItemDragController {
  AssetHandle asset_;

 public:
  AssetDragController(ui::AbstractGridView &view, const AssetHandle &asset);

  eWM_DragDataType get_drag_type() const override;
  void *create_drag_data() const override;
};

AssetView::AssetView(const AssetLibraryReference &library_ref,
                     const AssetShelfSettings &shelf_settings,
                     bContext &evil_C)
    : library_ref_(library_ref), shelf_settings_(shelf_settings), evil_C_(evil_C)
{
}

/* TODO calling a (.py defined) callback for every asset isn't exactly great. Should be a temporary
 * solution until there is proper filtering by asset traits. */
/**
 * Returns a vector of all asset shelf types displaying the given asset. That is, any asset shelf
 * type that has no poll function (all assets should be displayed), or whose
 * #AssetShelfType.asset_poll() function returns true.
 *
 * If the returned vector is not empty, the asset should be visible.
 */
static Vector<AssetShelfType *> asset_shelf_types_showing_asset(const SpaceType &space_type,
                                                                const bContext &C,
                                                                const AssetHandle &asset)
{
  Vector<AssetShelfType *> shelf_types;

  LISTBASE_FOREACH (AssetShelfType *, shelf_type, &space_type.asset_shelf_types) {
    if (!shelf_type->poll || !shelf_type->poll(&C, shelf_type)) {
      continue;
    }
    if (!shelf_type->asset_poll || shelf_type->asset_poll(shelf_type, &asset)) {
      shelf_types.append(shelf_type);
    }
  }

  return shelf_types;
}

static bool shelves_is_any_no_asset_drag(const Vector<AssetShelfType *> &shelves)
{
  for (AssetShelfType *shelf_type : shelves) {
    if (shelf_type->flag & ASSET_SHELF_TYPE_NO_ASSET_DRAG) {
      return true;
    }
  }

  return false;
}

void AssetView::build_items()
{
  const asset_system::AssetLibrary *library = ED_assetlist_library_get_once_available(
      library_ref_);
  if (!library) {
    return;
  }
  const SpaceLink *space_link = CTX_wm_space_data(&evil_C_);
  const SpaceType *space_type = BKE_spacetype_from_id(space_link->spacetype);

  ED_assetlist_iterate(library_ref_, [&](AssetHandle asset) {
    Vector<AssetShelfType *> shelves_showing_asset = asset_shelf_types_showing_asset(
        *space_type, evil_C_, asset);

    if (shelves_showing_asset.is_empty()) {
      return true;
    }

    const AssetMetaData *asset_data = ED_asset_handle_get_metadata(&asset);

    if (catalog_filter_ && !catalog_filter_->contains(asset_data->catalog_id)) {
      /* Skip this asset. */
      return true;
    }

    const bool show_names = (shelf_settings_.display_flag & ASSETSHELF_SHOW_NAMES);

    /* Use the path within the library as identifier, this should be unique. */
    const StringRef identifier = ED_asset_handle_get_relative_path(asset);
    const StringRef name = show_names ? ED_asset_handle_get_name(&asset) : "";
    const int preview_id = ED_asset_handle_get_preview_icon_id(&asset);

    AssetViewItem &item = add_item<AssetViewItem>(asset, identifier, name, preview_id);
    /* Disable asset dragging if any of the shelves requested it to be disabled. They often
     * override the drag behavior with custom operators. */
    if (shelves_is_any_no_asset_drag(shelves_showing_asset)) {
      item.disable_asset_drag();
    }

    return true;
  });
}

void AssetView::set_catalog_filter(
    const std::optional<asset_system::AssetCatalogFilter> &catalog_filter)
{
  if (catalog_filter) {
    catalog_filter_.emplace(*catalog_filter);
  }
  else {
    catalog_filter_ = std::nullopt;
  }
}

static std::optional<asset_system::AssetCatalogFilter> catalog_filter_from_shelf_settings(
    const AssetShelfSettings *shelf_settings, const asset_system::AssetLibrary &library)
{
  if (!shelf_settings || !shelf_settings->active_catalog_path) {
    return {};
  }

  asset_system ::AssetCatalog *active_catalog = library.catalog_service->find_catalog_by_path(
      shelf_settings->active_catalog_path);
  if (!active_catalog) {
    return {};
  }

  return library.catalog_service->create_catalog_filter(active_catalog->catalog_id);
}

/* ---------------------------------------------------------------------- */

AssetViewItem::AssetViewItem(const AssetHandle &asset,
                             StringRef identifier,
                             StringRef label,
                             int preview_icon_id)
    : ui::PreviewGridItem(identifier, label, preview_icon_id), asset_(asset)
{
}

void AssetViewItem::disable_asset_drag()
{
  allow_asset_drag_ = false;
}

void AssetViewItem::build_grid_tile(uiLayout &layout) const
{
  PointerRNA file_ptr;
  RNA_pointer_create(
      nullptr,
      &RNA_FileSelectEntry,
      /* XXX passing file pointer here, should be asset handle or asset representation. */
      const_cast<FileDirEntry *>(asset_.file_data),
      &file_ptr);

  uiBlock *block = uiLayoutGetBlock(&layout);
  UI_but_context_ptr_set(
      block, reinterpret_cast<uiBut *>(view_item_but_), "active_file", &file_ptr);
  ui::PreviewGridItem::build_grid_tile(layout);
}

void AssetViewItem::build_context_menu(bContext &C, uiLayout &column) const
{
  const SpaceLink *space_link = CTX_wm_space_data(&C);
  const SpaceType *space_type = BKE_spacetype_from_id(space_link->spacetype);
  Vector<AssetShelfType *> shelves_showing_asset = asset_shelf_types_showing_asset(
      *space_type, C, asset_);

  for (AssetShelfType *shelf : shelves_showing_asset) {
    shelf->draw_context_menu(&C, shelf, &asset_, &column);
  }
}

std::unique_ptr<ui::AbstractViewItemDragController> AssetViewItem::create_drag_controller() const
{
  return allow_asset_drag_ ? std::make_unique<AssetDragController>(get_view(), asset_) : nullptr;
}

/* ---------------------------------------------------------------------- */

void build_asset_view(uiLayout &layout,
                      const AssetLibraryReference &library_ref,
                      const AssetShelfSettings *shelf_settings,
                      const bContext &C,
                      ARegion &region)
{
  ED_assetlist_storage_fetch(&library_ref, &C);
  ED_assetlist_ensure_previews_job(&library_ref, &C);

  const asset_system::AssetLibrary *library = ED_assetlist_library_get_once_available(library_ref);
  if (!library) {
    return;
  }

  const float tile_width = ED_asset_shelf_default_tile_width();
  const float tile_height = ED_asset_shelf_default_tile_height();

  std::unique_ptr asset_view = std::make_unique<AssetView>(
      library_ref, *shelf_settings, const_cast<bContext &>(C));
  asset_view->set_catalog_filter(catalog_filter_from_shelf_settings(shelf_settings, *library));
  asset_view->set_tile_size(tile_width, tile_height);

  uiBlock *block = uiLayoutGetBlock(&layout);
  ui::AbstractGridView *grid_view = UI_block_add_view(
      *block, "asset shelf asset view", std::move(asset_view));

  ui::GridViewBuilder builder(*block);
  builder.build_grid_view(*grid_view, region.v2d, layout);
}

/* ---------------------------------------------------------------------- */
/* Dragging. */

AssetDragController::AssetDragController(ui::AbstractGridView &view, const AssetHandle &asset)
    : ui::AbstractViewItemDragController(view), asset_(asset)
{
}

eWM_DragDataType AssetDragController::get_drag_type() const
{
  const ID *local_id = ED_asset_handle_get_local_id(&asset_);
  return local_id ? WM_DRAG_ID : WM_DRAG_ASSET;
}

void *AssetDragController::create_drag_data() const
{
  ID *local_id = ED_asset_handle_get_local_id(&asset_);
  if (local_id) {
    return static_cast<void *>(local_id);
  }

  char asset_blend_path[FILE_MAX_LIBEXTRA];
  ED_asset_handle_get_full_library_path(&asset_, asset_blend_path);
  const eAssetImportMethod import_method = ED_asset_handle_get_import_method(&asset_).value_or(
      ASSET_IMPORT_APPEND_REUSE);

  AssetView &asset_view = get_view<AssetView>();
  return static_cast<void *>(WM_drag_create_asset_data(
      &asset_, BLI_strdup(asset_blend_path), import_method, &asset_view.evil_C_));
}

}  // namespace blender::ed::asset::shelf