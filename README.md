# Blueprint Diff & Edit

An Unreal Editor plugin for reviewing a source-control revision beside the editable Blueprint or Widget Blueprint. Inspect graph, Blueprint settings and widget layout changes while continuing to work in the native editor.

## Requirements

- Unreal Engine 5.6.1 on Windows was tested with Win64 Development Editor.
- Visual Studio 2022 with Desktop development with C++ and the Windows SDK for building from source.
- Git with the `--attr-source` option for Git history and master comparison. Install Git LFS when the repository tracks Blueprint assets through LFS. Git 2.54 and Git LFS 3.7 were tested.

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
3. Choose **Compare with master** to compare against the stored `origin/master` tracking ref, with a fallback to the local `master` branch when the tracking ref is absent. This uses local refs. Git LFS can download required asset objects.
4. Select a changed function, Event Graph, Construction Script, macro or Blueprint Settings in the left column. Pan or zoom either graph to move both views together. Drag the dividers to resize the panes. Sizes are shared across Blueprints in the project.
5. Right-click a changed top-level function and select **Revert Function to Compared Revision**. Revert blocks cases that would change other graphs or leave unresolved calls. Use Undo to reverse a revert and Save to write the working Blueprint.

In a Widget Blueprint, **Diff & Edit** is available in Designer and Graph. Select **Widgets** to inspect the hierarchy, properties and layout slots. The historical preview uses the available inherited layout and the revision's named-slot overrides. Selecting a widget that still exists locates it in the working Designer.

## License

Blueprint Diff & Edit is licensed under the MIT License. See [LICENSE](LICENSE). Unreal Engine is subject to the [Unreal Engine EULA](https://www.unrealengine.com/eula/unreal).
