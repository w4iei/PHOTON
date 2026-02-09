import pcbnew

class RotateSelection45(pcbnew.ActionPlugin):
    def defaults(self):
        self.name = "Rotate selection 45°"
        self.category = "Modify PCB"
        self.description = "Rotate currently selected items by 45° around their center"
        self.show_toolbar_button = True
        self.icon_file_name = ""  # Optional: path to PNG

    def _selection_iter(self, selection):
        """Return an iterator over the selection, or empty iterator on failure."""
        if selection is None:
            return iter(())
        try:
            return iter(selection)
        except TypeError:
            return iter(())

    def _selection_size(self, selection):
        """Return number of items in the selection, robust across KiCad versions."""
        if selection is None:
            return 0
        if hasattr(selection, "GetSize"):
            try:
                return selection.GetSize()
            except Exception:
                pass
        try:
            return len(selection)
        except Exception:
            return 0

    def _compute_bbox(self, selection):
        """
        Compute a simple bounding box (min_x, min_y, max_x, max_y)
        for all items in the selection that have GetBoundingBox().
        Coordinates are in internal KiCad units (nm).
        """
        min_x = None
        min_y = None
        max_x = None
        max_y = None

        for item in self._selection_iter(selection):
            if not hasattr(item, "GetBoundingBox"):
                continue
            try:
                bb = item.GetBoundingBox()
            except Exception:
                continue

            # Try to get x, y
            x0 = y0 = None
            if hasattr(bb, "GetX") and hasattr(bb, "GetY"):
                try:
                    x0 = bb.GetX()
                    y0 = bb.GetY()
                except Exception:
                    pass

            if x0 is None or y0 is None:
                # Try origin-based
                if hasattr(bb, "GetOrigin"):
                    try:
                        origin = bb.GetOrigin()
                        # origin is usually VECTOR2I / wxPoint-like
                        x0 = origin.x
                        y0 = origin.y
                    except Exception:
                        pass

            # Try to get width/height
            w = h = None
            if hasattr(bb, "GetWidth") and hasattr(bb, "GetHeight"):
                try:
                    w = bb.GetWidth()
                    h = bb.GetHeight()
                except Exception:
                    pass

            if (w is None or h is None) and hasattr(bb, "GetSize"):
                try:
                    size = bb.GetSize()
                    w = size.x
                    h = size.y
                except Exception:
                    pass

            # If we still don't have all coordinates, skip this item
            if x0 is None or y0 is None or w is None or h is None:
                continue

            x1 = x0 + w
            y1 = y0 + h

            if min_x is None or x0 < min_x:
                min_x = x0
            if min_y is None or y0 < min_y:
                min_y = y0
            if max_x is None or x1 > max_x:
                max_x = x1
            if max_y is None or y1 > max_y:
                max_y = y1

        if min_x is None:
            return None  # no valid bounding boxes

        return (min_x, min_y, max_x, max_y)

    def _make_anchor_point(self, cx, cy):
        """
        Create a KiCad-compatible point for Rotate().
        Tries VECTOR2I first, then wxPoint.
        """
        # KiCad 7+/9 typically has VECTOR2I
        if hasattr(pcbnew, "VECTOR2I"):
            try:
                return pcbnew.VECTOR2I(int(cx), int(cy))
            except Exception:
                pass

        # Fallback: wxPoint is available in many builds
        if hasattr(pcbnew, "wxPoint"):
            try:
                return pcbnew.wxPoint(int(cx), int(cy))
            except Exception:
                pass

        # Absolute last resort: some versions may accept a tuple
        return (int(cx), int(cy))

    def _make_angle_45deg(self):
        """
        Build an angle object (or raw value) corresponding to 45 degrees.
        """
        # Newer KiCad: EDA_ANGLE with explicit unit
        if hasattr(pcbnew, "EDA_ANGLE"):
            try:
                return pcbnew.EDA_ANGLE(45, pcbnew.DEGREES_T)
            except Exception:
                try:
                    # deci-degrees fallback
                    return pcbnew.EDA_ANGLE(45 * 10)
                except Exception:
                    pass

        # Some APIs accept plain deci-degrees as int
        return 45 * 10

    def Run(self):
        board = pcbnew.GetBoard()
        selection = pcbnew.GetCurrentSelection()

        if self._selection_size(selection) == 0:
            return  # nothing selected

        bbox = self._compute_bbox(selection)
        if bbox is None:
            return

        min_x, min_y, max_x, max_y = bbox
        cx = (min_x + max_x) // 2
        cy = (min_y + max_y) // 2

        anchor = self._make_anchor_point(cx, cy)
        angle = self._make_angle_45deg()

        # Rotate items
        for item in self._selection_iter(selection):
            if not hasattr(item, "Rotate"):
                continue
            try:
                item.Rotate(anchor, angle)
            except Exception:
                # Ignore items that can't be rotated
                pass

        pcbnew.Refresh()

RotateSelection45().register()