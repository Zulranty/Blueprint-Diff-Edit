# Blueprint Diff & Edit

An Unreal Editor plugin for reviewing a source-control revision beside the editable Blueprint or Widget Blueprint. Inspect graph, Blueprint settings and widget layout changes while continuing to work in the native editor.

## Requirements

- Unreal Engine 5.6.1 on Windows was tested with Win64 Development Editor.
- Visual Studio 2022 with Desktop development with C++ and the Windows SDK for building from source.
- Git with the `--attr-source` option for revision history and default branch comparison. The built-in Unreal Git provider is supported. Install Git LFS when the repository tracks Blueprint assets through LFS. Git 2.54 and Git LFS 3.7 were tested.

## Build and install into Unreal Engine

Run `BuildAndInstall.bat` from the checkout. It builds the plugin for Win64 using Unreal Automation Tool.

- From a checkout inside `Engine/Plugins`, the batch uses that engine and installs the build into the same plugin folder. The checkout keeps its original `.uplugin` and source files.
- From an external checkout, the batch finds the Epic Games Launcher installation of Unreal Engine 5.6 and installs to `Engine/Plugins/Marketplace/BlueprintDiffEdit`.

Keep one copy of Blueprint Diff & Edit per engine. If another copy is present, the batch prints its path and stops before building.

You can specify an engine installation on the command line or set `UE_ROOT`.

```bat
BuildAndInstall.bat "D:\Unreal\UE_5.6"
```

The script publishes the new module manifest after copying the package. Existing editor sessions can continue using their loaded DLL until restart.

## Install into a single project from source

Copy `BlueprintDiffEdit.uplugin` and the `Source/` directory into `<Project>/Plugins/BlueprintDiffEdit/`, then build the project's Editor target and open the project. If the engine already has the same plugin installed, use one installation for that project.

## Use the diff

1. Open a Blueprint and click **Diff & Edit** on the toolbar, or open it through the Content Browser context menu. This opens the comparison beside the standalone Blueprint editor.
2. Open the revision dropdown with Revision Control connected. With Git, the latest available snapshot can open while the list loads. The list follows file renames, and entries show the revision ID, subject, author and date when available.
3. Choose **Compare with default branch** to use the local `origin/HEAD` ref, with fallbacks to tracked and local `master` or `main`. Git LFS can download required asset objects.
4. Select a changed function, Event Graph, Construction Script, macro or Blueprint Settings in the left column. Pan or zoom either graph to move both views together. Drag the dividers to resize the panes. Sizes are shared across Blueprints in the project.

In **Blueprint Settings**, selecting a variable or event dispatcher also selects it in the working editor's **My Blueprint** panel. Edit the working default directly below the change list. Right-click a setting and choose **Revert to Compared Revision** to restore it. The action supports Undo. A struct or collection is restored as one property.
5. Right-click a changed top-level function and select **Revert Function to Compared Revision**. Revert blocks cases that would change other graphs or leave unresolved calls. Use Undo to reverse a revert and Save to write the working Blueprint.

In a Widget Blueprint, **Diff & Edit** is available in Designer and Graph. Select **Widgets** to inspect the hierarchy, properties and layout slots. The historical preview uses the available inherited layout and the revision's named-slot overrides. Selecting a widget that still exists locates it in the working Designer.

## Browse changed files

Open **Tools → Revision Control → Git Changes** in the Level Editor. The list shows `.uasset` and C/C++ changes in the current project's Content, Source and Plugins folders. Use **All**, **Staged**, or **Working**, and search by file name or path. Resize or hide the filter sidebar. Double-click a row to open the file. The diff shows the same list below the changed functions and highlights its current asset. Double-clicking a Blueprint there opens its editable diff.

Right-click an asset and select **Browse to Asset** to highlight the asset in the Content Browser. **Stage File** stages the selected file's saved changes. **Revert File to HEAD** discards its staged and working changes after confirmation. The menu offers a separate removal action for untracked files.

## Compare materials

Open **Material Diff** from the Content Browser context menu or the material editor toolbar. Choose a Git revision on the left and edit the working asset in the native editor on the right. Material graph edits appear in the comparison before **Apply**. Material Instances show differences in settings and overrides alongside their parameter editor. Select a change to locate its node or see the previous and current property values. Use the editor's **Apply** and **Save** actions as usual. **Close Diff** returns to the material editor.

## License

Blueprint Diff & Edit is licensed under the MIT License. See [LICENSE](LICENSE). Unreal Engine is subject to the [Unreal Engine EULA](https://www.unrealengine.com/eula/unreal).
