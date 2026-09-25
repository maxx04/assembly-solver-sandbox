# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any, Final

from Base.Metadata import constmethod, export

from App.Part import Part
from App.DocumentObject import DocumentObject

@export(Include="Mod/Assembly/App/AssemblyObject.h", Namespace="Assembly")
class AssemblyObject(Part):
    """
    This class handles document objects in Assembly

    Author: Ondsel (development@ondsel.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def solve(self, enableUndo: bool = False, /) -> int:
        """
        Solve the assembly and update part placements.

        Args:
        enableRedo: Whether the solve save the initial position of parts
        to enable undoing it even without a transaction.
        Defaults to `False` ie the solve cannot be undone if called
        outside of a transaction.

        Returns:
        0 in case of success, otherwise the following codes in this order of
        priority:
        -6 if no parts are fixed.
        -4 if over-constrained,
        -3 if conflicting constraints,
        -5 if malformed constraints
        -1 if solver error,
        -2 if redundant constraints.
        """
        ...

    @constmethod
    def generateSimulation(self, simulationObject: DocumentObject, /) -> int:
        """
        Generate the simulation.

        Args:
        simulationObject: The simulation Object.

        Returns:
        0 in case of success, otherwise the following codes in this order of
        priority:
        -6 if no parts are fixed.
        -4 if over-constrained,
        -3 if conflicting constraints,
        -5 if malformed constraints
        -1 if solver error,
        -2 if redundant constraints.
        """
        ...

    @constmethod
    def updateForFrame(self, index: int, /) -> None:
        """
        Update entire assembly to frame number specified.

        Args:
            index: index of frame.

        Returns: None
        """
        ...

    @constmethod
    def numberOfFrames(self) -> int:
        """Return Number of frames"""
        ...

    @constmethod
    def updateSolveStatus(self) -> Any:
        """updateSolveStatus()

        Args: None

        Returns: None"""
        ...

    @constmethod
    def undoSolve(self) -> Any:
        """Undo the last solve of the assembly and return part placements to their initial position.

        undoSolve()

        Returns: None"""
        ...

    @constmethod
    def ensureIdentityPlacements(self) -> None:
        """
        Makes sure that LinkGroups or sub-assemblies have identity placements.
        """
        ...

    @constmethod
    def clearUndo(self) -> None:
        """
        Clear the registered undo positions.
        """
        ...

    @constmethod
    def isPartConnected(self, obj: DocumentObject, /) -> bool:
        """
        Check if a part is connected to the ground through joints.
        Returns: True if part is connected to ground.
        """
        ...

    @constmethod
    def exportIdentityGraphDot(self) -> str:
        """
        Export the assembly's IdentityGraph (parts/instances as nodes, joints as edges,
        grounded parts highlighted) as Graphviz DOT text.

        Returns: DOT source, e.g. to render with `dot -Tsvg` or an online Graphviz viewer.
        """
        ...

    @constmethod
    def verifyIdentityGraphAgainstAsmt(self) -> str:
        """
        Cross-check the IdentityGraph against a fresh ASMT export (the same real solver-feeding
        pipeline used by "Export ASMT File"): compares the graph's own rigid-merged body count
        and joint edge count against what actually reaches the solver. A diagnostic aid for
        debugging identity-resolution issues, not an automated correctness test - a mismatch in
        body count points at a real divergence, the joint count comparison is informational only
        (a single FreeCAD joint can translate into several MbD joint primitives).

        Returns: human-readable report text.
        """
        ...

    @constmethod
    def isJointConnectingPartToGround(self, joint: DocumentObject, prop_name: str, /) -> Any:
        """
        Check if a joint is connecting a part to the ground.

        Args:
        - joint: document object of the joint to check.
        - prop_name: string 'Part1' or 'Part2' of the joint.

        Returns: True if part is connected to ground.
        """
        ...

    @constmethod
    def isPartGrounded(self, obj: DocumentObject, /) -> Any:
        """
        Check if a part has a grounded joint.

        Args:
        - obj: document object of the part to check.

        Returns: True if part has grounded joint.
        """
        ...

    @constmethod
    def verifyIdentityGraphEquivalence(self) -> Any:
        """
        FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 0, siehe
        /home/maxx/.claude/plans/enumerated-roaming-river.md): rein diagnostisch, temporaer fuer
        die Verifikations-Testmatrix - vergleicht IdentityGraph::resolve()/resolveJointRef() gegen
        canonicalizeForMbD()/resolveJointReference() fuer jedes geerdete Teil und jede
        Joint-Referenz dieser Baugruppe.

        Returns: Liste von Strings, je Zeile "MATCH ...", "DIVERGENCE (by design, dupliziert) ..."
        (erwartete Abweichung bei Instanz-Duplikation) oder "MISMATCH ..." (unerwartete Abweichung).
        """
        ...

    @constmethod
    def exportAsASMT(self, file_name: str, /) -> None:
        """
        Export the assembly in a text format called ASMT.

        Args:
        - fileName: The name of the file where the ASMT will be exported.
        """
        ...

    @constmethod
    def getDownstreamParts(
        self, start_part: DocumentObject, joint_to_ignore: DocumentObject, /
    ) -> list[DocumentObject]:
        """
        Finds all parts connected to a start_part that are not connected to ground
        when a specific joint is ignored.

        This is used to find the entire rigid group of unconstrained components that
        should be moved together during a pre-solve operation or a drag.

        Args:
            start_part: The App.DocumentObject to begin the search from.
            joint_to_ignore: The App.DocumentObject (a joint) to temporarily
                             suppress during the connectivity check.

        Returns:
            A list of App.DocumentObject instances representing the downstream parts.
        """
        ...

    @constmethod
    def preDrag(self, drag_parts: list[DocumentObject], /) -> None:
        """
        Prepare the assembly for an interactive drag, headless/scriptable equivalent of what
        ViewProviderAssembly::tryInitMove() triggers for a real mouse-down (see
        docs/ARCHITECTURE.md #2.2).

        Runs one full solve() (with fixed-joint bundling enabled) to (re)build the MbD model,
        then determines which of drag_parts will actually be driven independently in MbD during
        the following doDragStep() calls (see AssemblyObject.h's dragRigidLeader/
        dragRigidFollowerCache/draggedParts members for the current, evolving rules).

        Args:
            drag_parts: the App.DocumentObject instances to drag - pass the SAME list
                        ViewProviderAssembly::findDragMode() would compute (typically the moving
                        joint's part plus everything getDownstreamParts() returns for it), in
                        the SAME order (the first entry is treated as the leading/grounding-
                        connected body).

        For a scripted drag test: call this once, then repeatedly set the Placement property of
        each drag_parts entry by the SAME per-frame delta (mirroring what
        ViewProviderAssembly::tryMouseMove() does for a real mouse move) and call doDragStep()
        after each change; call postDrag() once at the end (mouse-up equivalent).
        """
        ...

    @constmethod
    def doDragStep(self) -> None:
        """
        Run one interactive-drag solve step (headless/scriptable equivalent of what a single
        mouseMove() event triggers via ViewProviderAssembly::tryMouseMove(), see
        docs/ARCHITECTURE.md #2.2).

        Reads the CURRENT Placement of whichever parts preDrag() decided to drive independently,
        feeds them into the MbD solver as high-weight targets (see
        PosICDragNewtonRaphson::initializeGlobally()), runs one drag solve, and - if the result
        validates - applies it to the document (including the rigid-transport/pin corrections
        for the parts preDrag() did NOT drive independently).

        Must be called after preDrag(); has no effect (raises no error but does nothing useful)
        if preDrag() was never called or postDrag() already ended the drag.
        """
        ...

    @constmethod
    def postDrag(self) -> None:
        """
        End an interactive drag (headless/scriptable equivalent of a mouse-up after
        preDrag()/doDragStep(), see docs/ARCHITECTURE.md #2.2). Lets the MbD solver do its own
        post-drag bookkeeping and marks the object as touched.
        """
        ...
    Joints: Final[list]
    """A list of all joints this assembly has."""
