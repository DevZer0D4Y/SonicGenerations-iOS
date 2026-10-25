// The startup mod manager, ported from the UnleashedRecompiled iOS port.

#include "mod_manager_ios.h"

#include "mods/mod_database.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

static NSString* const kTitle = @"Mods — top wins conflicts";

static NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

@interface SgModManager : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^completion)(void);
// Opened from the in-game MODS button: mods load at startup, so changes wait
// for the next launch.
@property(nonatomic) BOOL inGame;
@end

@implementation SgModManager {
  std::filesystem::path _root;
  std::vector<sg::mods::ModEntry> _mods;
  bool _scanFailed;
  std::vector<std::pair<std::filesystem::path, bool>> _initial;
}

- (instancetype)initWithRoot:(std::filesystem::path)root {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) _root = std::move(root);
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = kTitle;
  self.tableView.allowsSelectionDuringEditing = YES;
  self.tableView.editing = YES;
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 76;
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Import folder"
      style:UIBarButtonItemStylePlain target:self action:@selector(importFolder)];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
      initWithTitle:self.inGame ? @"Done" : @"Play"
              style:UIBarButtonItemStyleDone target:self action:@selector(play)];
  UILabel* footer = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 600, 150)];
  footer.numberOfLines = 0;
  footer.textAlignment = NSTextAlignmentCenter;
  footer.text = @"Tap to enable or disable; drag to reorder. Import an extracted HedgeModManager mod "
                @"folder for Sonic Generations (the one with mod.ini), or copy it into Files → "
                @"Sonic Generations → mods. A mod's DLL or code file is Windows PC code: it is "
                @"skipped and only the mod's files load, so its new moves and options are missing.\n"
                @"Changes apply the next time the game starts.";
  footer.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  footer.textColor = UIColor.secondaryLabelColor;
  self.tableView.tableFooterView = footer;
  [self reloadMods];
  _initial = [self selection];
}

// Which mods are enabled, in order: what the game loads.
- (std::vector<std::pair<std::filesystem::path, bool>>)selection {
  std::vector<std::pair<std::filesystem::path, bool>> selection;
  for (const auto& mod : _mods) selection.emplace_back(mod.ini_path, mod.enabled);
  return selection;
}

- (void)showError:(NSString*)message {
  UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Mod manager"
      message:message preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)reloadMods {
  try {
    _mods = sg::mods::Scan(_root);
    _scanFailed = false;
  } catch (const std::exception& ex) {
    _scanFailed = true;
    [self showError:ToNS(ex.what())];
  }
  [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView { return 1; }
- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  return _mods.empty() ? @"No mods installed" : @"Installed mods";
}
- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  return NSInteger(_mods.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
  const auto& mod = _mods[size_t(indexPath.row)];
  std::string title = mod.title;
  if (!mod.version.empty()) title += "  " + mod.version;
  cell.textLabel.text = ToNS(title);
  cell.textLabel.numberOfLines = 2;
  std::string subtitle;
  if (!mod.issue.empty()) {
    subtitle = mod.issue;
  } else {
    subtitle = mod.enabled ? "Enabled" : "Disabled";
    if (!mod.author.empty()) subtitle += " • " + mod.author;
    if (!mod.note.empty()) subtitle += "\n" + mod.note;
  }
  cell.detailTextLabel.text = ToNS(subtitle);
  cell.detailTextLabel.numberOfLines = 0;
  cell.accessoryType = mod.enabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.textLabel.textColor = mod.issue.empty() ? UIColor.labelColor : UIColor.secondaryLabelColor;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  auto& mod = _mods[size_t(indexPath.row)];
  if (mod.issue.empty()) {
    mod.enabled = !mod.enabled;
  } else {
    [self showError:ToNS(mod.issue)];
  }
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  [tableView reloadRowsAtIndexPaths:@[indexPath] withRowAnimation:UITableViewRowAnimationNone];
}

- (BOOL)tableView:(UITableView*)tableView canMoveRowAtIndexPath:(NSIndexPath*)indexPath { return YES; }
- (UITableViewCellEditingStyle)tableView:(UITableView*)tableView editingStyleForRowAtIndexPath:(NSIndexPath*)indexPath {
  return UITableViewCellEditingStyleNone;
}
- (BOOL)tableView:(UITableView*)tableView shouldIndentWhileEditingRowAtIndexPath:(NSIndexPath*)indexPath {
  return NO;
}
- (void)tableView:(UITableView*)tableView moveRowAtIndexPath:(NSIndexPath*)source toIndexPath:(NSIndexPath*)destination {
  auto entry = std::move(_mods[size_t(source.row)]);
  _mods.erase(_mods.begin() + source.row);
  _mods.insert(_mods.begin() + destination.row, std::move(entry));
}

- (BOOL)saveMods {
  if (_scanFailed) {
    [self showError:@"The mods folder could not be scanned. Fix its permissions using Files and relaunch."];
    return NO;
  }
  std::string error;
  if (sg::mods::Save(_root, _mods, error)) return YES;
  [self showError:ToNS(error)];
  return NO;
}

- (void)play {
  if (![self saveMods]) return;
  auto close = ^{
    [self.navigationController dismissViewControllerAnimated:YES completion:^{
      if (self.completion) self.completion();
    }];
  };
  if (!self.inGame || [self selection] == _initial) {
    close();
    return;
  }
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:@"Restart to apply"
                       message:@"Mods load when the game starts. Quit now and open the game "
                               @"again to play with the new selection, or keep playing and "
                               @"it applies next time."
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Keep Playing"
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction*) { close(); }]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Quit Game"
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction*) { std::exit(0); }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)importFolder {
  if (![self saveMods]) return;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)setBusy:(BOOL)busy {
  self.tableView.userInteractionEnabled = !busy;
  self.title = busy ? @"Importing mod…" : kTitle;
  self.navigationItem.leftBarButtonItem.enabled = !busy;
  self.navigationItem.rightBarButtonItem.enabled = !busy;
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject;
  if (!url || !url.isFileURL) return;
  BOOL scoped = [url startAccessingSecurityScopedResource];
  [self setBusy:YES];
  const auto root = _root;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::string error;
    const bool success = sg::mods::Import(root, std::filesystem::path(url.path.UTF8String), error);
    if (scoped) [url stopAccessingSecurityScopedResource];
    NSString* message = ToNS(error);
    dispatch_async(dispatch_get_main_queue(), ^{
      [self setBusy:NO];
      [self reloadMods];
      if (!success) [self showError:message];
    });
  });
}
@end

void SgRunModManager(std::filesystem::path root, std::function<void()> done, bool in_game) {
  // Retained until Play; the completion runs once.
  static SgModManager* manager;
  static std::function<void()> finish;
  finish = std::move(done);
  UIViewController* presenter = nil;
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:UIWindowScene.class]) continue;
    for (UIWindow* window in ((UIWindowScene*)scene).windows) {
      if (window.isKeyWindow) presenter = window.rootViewController;
    }
    if (presenter) break;
  }
  while (presenter.presentedViewController) presenter = presenter.presentedViewController;
  if (!presenter) {
    // No window to show it on: start with the saved selection.
    auto callback = std::move(finish);
    callback();
    return;
  }
  manager = [[SgModManager alloc] initWithRoot:std::move(root)];
  manager.inGame = in_game;
  manager.completion = ^{
    manager = nil;
    auto callback = std::move(finish);
    if (callback) callback();
  };
  UINavigationController* navigation = [[UINavigationController alloc] initWithRootViewController:manager];
  navigation.modalPresentationStyle = UIModalPresentationFullScreen;
  navigation.modalInPresentation = YES;
  [presenter presentViewController:navigation animated:NO completion:nil];
}

