#!/usr/bin/env python3
"""Checks that the crash loop can detect a missing durability step.

Each mutant removes one sync or directory sync from the engine. For each,
this builds the debug preset, runs the crash loop for 3000 cycles, and
reports whether the loop failed. Every mutant should be CAUGHT; one that is
not means the crash loop has a blind spot.

The source tree is restored after each mutant. Run from a clean tree:

    python3 test/tools/mutation_check.py          # all mutants
    python3 test/tools/mutation_check.py 0 4      # only these
"""
import subprocess, sys, shutil, os
repo=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
mutants=[
 ('no SyncDir before the manifest edit (new tables/logs not durable)', 'src/db/version_set.cc',
  '  if (s.ok()) s = env_->SyncDir(dbname_);\n  if (s.ok()) {\n    std::string record;', '  if (s.ok()) {\n    std::string record;'),
 ('no SyncDir after renaming CURRENT', 'src/db/filename.cc',
  '  if (s.ok()) s = env->SyncDir(dbname);\n  if (!s.ok()) (void)env->RemoveFile(tmp);', '  if (!s.ok()) (void)env->RemoveFile(tmp);'),
 ('outgoing log not synced at memtable switch', 'src/db/db_impl.cc',
  '  if (s.ok()) s = logfile_->Sync();\n  if (s.ok()) s = logfile_->Close();\n  // And make', '  if (s.ok()) s = logfile_->Close();\n  // And make'),
 ('no SyncDir for the new log at memtable switch', 'src/db/db_impl.cc',
  '  if (s.ok()) s = env_->SyncDir(dbname_);\n  lock->lock();\n\n  if (!s.ok()) {\n    // The old log may be closed', '  lock->lock();\n\n  if (!s.ok()) {\n    // The old log may be closed'),
 ('sync=true writes not synced', 'src/db/db_impl.cc',
  '    if (status.ok() && options.sync) {\n      status = logfile_->Sync();\n    }', ''),
 ('flushed table not synced', 'src/db/builder.cc',
  '  if (s.ok()) s = file->Sync();\n', ''),
 ('compaction output not synced', 'src/db/db_impl.cc',
  '  if (s.ok()) s = compact->outfile->Sync();\n', ''),
 ('manifest record not synced', 'src/db/version_set.cc',
  '  if (s.ok()) s = descriptor_file_->Sync();\n  if (s.ok() && !new_manifest_file.empty()) {', '  if (s.ok() && !new_manifest_file.empty()) {'),
]
only = sys.argv[1:] 
for i,(name,path,old,new) in enumerate(mutants):
    if only and str(i) not in only: continue
    full=os.path.join(repo,path)
    orig=open(full).read()
    assert old in orig, (name, 'pattern not found')
    open(full,'w').write(orig.replace(old,new,1))
    try:
        b=subprocess.run(['cmake','--build','--preset','debug'],cwd=repo,capture_output=True,text=True)
        if b.returncode!=0:
            print(f'[{i}] {name}: BUILD FAILED'); print(b.stdout[-800:]); continue
        env=dict(os.environ, LSM_CRASH_CYCLES='3000', LSM_CRASH_SEED='7')
        r=subprocess.run([repo+'/build/debug/lsm_tests','--gtest_filter=CrashLoop.EveryAcknowledged*'],cwd=repo,capture_output=True,text=True,env=env,timeout=900)
        out=r.stdout+r.stderr
        caught = r.returncode!=0
        reason=''
        for line in out.splitlines():
            if 'lost a write' in line or 'did not reopen' in line or 'Actual: false' in line or 'state is not' in line or 'hole' in line or 'invariant' in line:
                reason=line.strip()[:230]; break
        print(f'[{i}] {name}: {"CAUGHT" if caught else "NOT CAUGHT"}  {reason}')
    finally:
        open(full,'w').write(orig)
subprocess.run(['cmake','--build','--preset','debug'],cwd=repo,capture_output=True)
print('restored and rebuilt')
