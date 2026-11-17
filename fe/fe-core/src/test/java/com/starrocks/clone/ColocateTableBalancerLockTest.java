// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.clone;

import com.google.common.collect.Lists;
import com.starrocks.alter.SystemHandler;
import com.starrocks.catalog.ColocateTableIndex;
import com.starrocks.catalog.ColocateTableIndex.GroupId;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.LocalTablet;
import com.starrocks.catalog.LocalTablet.TabletHealthStatus;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Partition;
import com.starrocks.clone.ColocateMatchResult.Status;
import com.starrocks.common.Config;
import com.starrocks.common.Pair;
import com.starrocks.common.jmockit.Deencapsulation;
import com.starrocks.common.lock.LockTestUtils;
import com.starrocks.common.util.concurrent.lock.LockManager;
import com.starrocks.common.util.concurrent.lock.LockType;
import com.starrocks.common.util.concurrent.lock.Locker;
import com.starrocks.common.util.concurrent.lock.YieldableLock;
import com.starrocks.leader.TabletCollector;
import com.starrocks.load.routineload.RoutineLoadTaskScheduler;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.utframe.StarRocksAssert;
import com.starrocks.utframe.UtFrameUtils;
import mockit.Mock;
import mockit.MockUp;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public class ColocateTableBalancerLockTest {
    private static final ColocateTableBalancer BALANCER = ColocateTableBalancer.getInstance();
    private static StarRocksAssert starRocksAssert;
    private static ConnectContext connectContext;

    private final int origPartitionBatchNum = Config.tablet_checker_partition_batch_num;
    private final int origLockTimePerCycleMs = Config.tablet_checker_lock_time_per_cycle_ms;

    @BeforeAll
    public static void beforeClass() throws Exception {
        new MockUp<ColocateTableBalancer>() {
            @Mock
            protected void runAfterLeaseValid() {
            }
        };
        new MockUp<SystemHandler>() {
            @Mock
            protected void runAfterLeaseValid() {
            }
        };
        new MockUp<RoutineLoadTaskScheduler>() {
            @Mock
            protected void runAfterLeaseValid() {
            }
        };
        new MockUp<TabletChecker>() {
            @Mock
            protected void runAfterLeaseValid() {
            }
        };

        UtFrameUtils.createMinStarRocksCluster();
        GlobalStateMgr.getCurrentState().getAlterJobMgr().stop();
        GlobalStateMgr.getCurrentState().getHeartbeatMgr().setStop();
        GlobalStateMgr.getCurrentState().getTabletScheduler().setStop();
        TabletCollector collector = (TabletCollector) Deencapsulation.getField(
                GlobalStateMgr.getCurrentState(), "tabletCollector");
        collector.setStop();
        connectContext = UtFrameUtils.createDefaultCtx();
        starRocksAssert = new StarRocksAssert(connectContext);
    }

    @AfterEach
    public void tearDown() {
        Config.tablet_checker_partition_batch_num = origPartitionBatchNum;
        Config.tablet_checker_lock_time_per_cycle_ms = origLockTimePerCycleMs;
        LockTestUtils.assertNoLockInvariantViolations();
    }

    @Test
    public void testDoMatchOneGroupAllowsConcurrentWriteOnUnrelatedTableInSameDb() throws Exception {
        starRocksAssert.withDatabase("db_concurrent_write").useDatabase("db_concurrent_write")
                .withTable("CREATE TABLE db_concurrent_write.t_colocate(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 1 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_cw');")
                .withTable("CREATE TABLE db_concurrent_write.t_other(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 1 " +
                        "properties('replication_num' = '1');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_concurrent_write");
        OlapTable colocateTbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_colocate");
        OlapTable otherTbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_other");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(colocateTbl.getId());

        try {
            CountDownLatch insideTabletCheck = new CountDownLatch(1);
            CountDownLatch resumeTabletCheck = new CountDownLatch(1);
            AtomicBoolean heldDbIntentionShared = new AtomicBoolean(false);
            AtomicBoolean heldDbPlainRead = new AtomicBoolean(false);
            AtomicBoolean heldTableRead = new AtomicBoolean(false);

            new MockUp<TabletChecker>() {
                @Mock
                public TabletHealthStatus getColocateTabletHealthStatus(LocalTablet localTablet, long visibleVersion,
                                                                        int replicationNum, Set<Long> backendsSet) {
                    LockManager lockManager = GlobalStateMgr.getCurrentState().getLockManager();
                    Locker probe = new Locker();
                    heldDbIntentionShared.set(lockManager.isOwner(db.getId(), probe, LockType.INTENTION_SHARED));
                    heldDbPlainRead.set(lockManager.isOwner(db.getId(), probe, LockType.READ));
                    heldTableRead.set(lockManager.isOwner(colocateTbl.getId(), probe, LockType.READ));
                    insideTabletCheck.countDown();
                    try {
                        resumeTabletCheck.await(10, TimeUnit.SECONDS);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                    return TabletHealthStatus.HEALTHY;
                }
            };

            AtomicBoolean writerSucceeded = new AtomicBoolean(false);
            AtomicReference<Throwable> matchError = new AtomicReference<>();
            Thread matchThread = new Thread(() -> {
                try {
                    Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                            groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
                } catch (Throwable t) {
                    matchError.set(t);
                }
            });
            matchThread.start();
            Assertions.assertTrue(insideTabletCheck.await(5, TimeUnit.SECONDS));

            // While doMatchOneGroup holds the lock on t_colocate, acquire WRITE lock on t_other in the same DB
            Thread writerThread = new Thread(() -> {
                Locker writerLocker = new Locker();
                if (writerLocker.tryLockTablesWithIntensiveDbLock(
                        db.getId(), Lists.newArrayList(otherTbl.getId()), LockType.WRITE, 1000,
                        TimeUnit.MILLISECONDS)) {
                    try {
                        writerSucceeded.set(true);
                    } finally {
                        writerLocker.unLockTablesWithIntensiveDbLock(
                                db.getId(), Lists.newArrayList(otherTbl.getId()), LockType.WRITE);
                    }
                }
            });
            writerThread.start();
            writerThread.join(3000);
            resumeTabletCheck.countDown();
            matchThread.join(5000);

            Assertions.assertFalse(matchThread.isAlive(), "matchThread should have completed");
            Assertions.assertNull(matchError.get(), () -> "Unexpected error on matchThread: " + matchError.get());
            Assertions.assertTrue(heldDbIntentionShared.get(), "Expected INTENTION_SHARED on DB");
            Assertions.assertFalse(heldDbPlainRead.get(), "Must not hold plain READ lock on DB");
            Assertions.assertTrue(heldTableRead.get(), "Expected READ lock on colocate table");
            Assertions.assertTrue(writerSucceeded.get(),
                    "Concurrent WRITE on unrelated table in same DB must not block");
        } finally {
            colocateIndex.removeTable(colocateTbl.getId(), colocateTbl, false);
        }
    }

    @Test
    public void testUrgentPassSkipsLockWhenNoTableIsUrgent() throws Exception {
        starRocksAssert.withDatabase("db_urgent_skip").useDatabase("db_urgent_skip")
                .withTable("CREATE TABLE db_urgent_skip.t_non_urgent(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 1 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_urgent_skip');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_urgent_skip");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_non_urgent");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "matchOneGroupUrgent",
                    groupId, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());

            Assertions.assertEquals(Status.STABLE, result.status);
            Assertions.assertEquals(0L, result.lockTotalTime,
                    "Urgent pass must acquire zero locks when no table in the group is urgent");
        } finally {
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testBatchAndTimeBasedYieldingNotBypassedByUrgentFilter() throws Exception {
        starRocksAssert.withDatabase("db_yield").useDatabase("db_yield")
                .withTable("CREATE TABLE db_yield.t_part(k1 INT NOT NULL) DUPLICATE KEY(k1)\n" +
                        "PARTITION BY RANGE(k1) (\n" +
                        "  PARTITION p1 VALUES LESS THAN ('10'),\n" +
                        "  PARTITION p2 VALUES LESS THAN ('20'),\n" +
                        "  PARTITION p3 VALUES LESS THAN ('30')\n" +
                        ")\n" +
                        "DISTRIBUTED BY HASH(k1) BUCKETS 1\n" +
                        "PROPERTIES('replication_num' = '1', 'colocate_with' = 'grp_yield');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_yield");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_part");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            Partition p1 = tbl.getPartition("p1");
            Partition p3 = tbl.getPartition("p3");

            AtomicInteger refreshCount = new AtomicInteger(0);
            AtomicLong mockedHoldTimeMs = new AtomicLong(0L);
            new MockUp<YieldableLock>() {
                @Mock
                public long getCurrentHoldTimeMs() {
                    return mockedHoldTimeMs.get();
                }

                @Mock
                public void refresh(mockit.Invocation inv) {
                    refreshCount.incrementAndGet();
                    inv.proceed();
                }
            };

            // Case 1: p1 and p3 are urgent, p2 is not. With partitionBatchNum = 2,
            // skipping p2 must not prevent p3 (the 2nd matched partition) from triggering refresh().
            Config.tablet_checker_partition_batch_num = 2;
            Config.tablet_checker_lock_time_per_cycle_ms = 60000;
            new MockUp<TabletChecker>() {
                @Mock
                public boolean isUrgentTable(long dbId, long tblId) {
                    return tblId == tbl.getId();
                }

                @Mock
                public boolean isPartitionUrgent(long dbId, long tblId, long partId) {
                    return partId == p1.getId() || partId == p3.getId();
                }
            };

            Deencapsulation.invoke(BALANCER, "matchOneGroupUrgent",
                    groupId, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(1, refreshCount.get(),
                    "Expected exactly 1 batch refresh on the 2nd matched urgent partition");

            // Case 2: Time-based yielding respects MIN_LOCK_HOLD_TIME_MS (100ms floor) even when config is 10ms
            refreshCount.set(0);
            mockedHoldTimeMs.set(50L);
            Config.tablet_checker_partition_batch_num = 1000;
            Config.tablet_checker_lock_time_per_cycle_ms = 10;

            Deencapsulation.invoke(BALANCER, "matchOneGroupUrgent",
                    groupId, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(0, refreshCount.get(),
                    "Hold time 50ms must not trigger refresh when clamped to 100ms floor");

            mockedHoldTimeMs.set(150L);
            Deencapsulation.invoke(BALANCER, "matchOneGroupUrgent",
                    groupId, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(2, refreshCount.get(),
                    "Expected time-based refresh on each of the 2 matched urgent partitions at 150ms");
        } finally {
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testRevalidationWhenTableDroppedDuringBlockingAddYield() throws Exception {
        starRocksAssert.withDatabase("db_reval").useDatabase("db_reval")
                .withTable("CREATE TABLE db_reval.t_drop(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 2 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_reval');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_reval");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_drop");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            // Set bucket sequence to a non-existent backend so tablet status is COLOCATE_MISMATCH
            colocateIndex.addBackendsPerBucketSeq(groupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L), Lists.newArrayList(99999999L)));

            new MockUp<LocalTablet>() {
                @Mock
                public boolean readyToBeRepaired(TabletHealthStatus status, TabletSchedCtx.Priority priority) {
                    return true;
                }
            };

            AtomicInteger blockingAddCalls = new AtomicInteger(0);
            new MockUp<TabletScheduler>() {
                @Mock
                public Pair<Boolean, Long> blockingAddTabletCtxToScheduler(TabletSchedCtx ctx, boolean force,
                                                                           YieldableLock heldLock)
                        throws Exception {
                    blockingAddCalls.incrementAndGet();
                    // Yield lock via sleepUnlocked and drop the table while unlocked
                    heldLock.sleepUnlocked(1);
                    db.unprotectDropTable(tbl.getId(), true, false);
                    colocateIndex.removeTable(tbl.getId(), tbl, false);
                    return Pair.create(true, 10L);
                }
            };

            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                    groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertNotNull(result);
            Assertions.assertEquals(Status.UNKNOWN, result.status);
            Assertions.assertTrue(result.lockTotalTime > 0L);
            Assertions.assertEquals(1, blockingAddCalls.get(),
                    "Must stop checking remaining tablets of dropped table after post-yield re-validation");
        } finally {
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testRevalidationSkipsDeColocatedFirstTableAndContinuesToSecondTable() throws Exception {
        starRocksAssert.withDatabase("db_reval_two_tbls").useDatabase("db_reval_two_tbls")
                .withTable("CREATE TABLE db_reval_two_tbls.t1(k1 INT NOT NULL) DUPLICATE KEY(k1)\n" +
                        "PARTITION BY RANGE(k1) (\n" +
                        "  PARTITION p1 VALUES LESS THAN ('10'),\n" +
                        "  PARTITION p2 VALUES LESS THAN ('20')\n" +
                        ")\n" +
                        "DISTRIBUTED BY HASH(k1) BUCKETS 1\n" +
                        "PROPERTIES('replication_num' = '1', 'colocate_with' = 'grp_two_tbls');")
                .withTable("CREATE TABLE db_reval_two_tbls.t2(k1 INT NOT NULL) DUPLICATE KEY(k1)\n" +
                        "DISTRIBUTED BY HASH(k1) BUCKETS 1\n" +
                        "PROPERTIES('replication_num' = '1', 'colocate_with' = 'grp_two_tbls');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_reval_two_tbls");
        OlapTable t1 = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t1");
        OlapTable t2 = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t2");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(t1.getId());

        try {
            // Set bucket sequence to a non-existent backend so any checked partition records an unbalanced BalanceStat
            colocateIndex.addBackendsPerBucketSeq(groupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L)));
            Config.tablet_checker_partition_batch_num = 1;

            AtomicBoolean deColocatedT1 = new AtomicBoolean(false);
            new MockUp<YieldableLock>() {
                @Mock
                public void refresh(mockit.Invocation inv) {
                    inv.proceed();
                    if (deColocatedT1.compareAndSet(false, true)) {
                        // De-colocate t1 during the first refresh while t2 stays in the group
                        colocateIndex.removeTable(t1.getId(), t1, false);
                    }
                }
            };

            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                    groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(Status.UNSTABLE, result.status);
            for (Partition p : t1.getPartitions()) {
                Assertions.assertTrue(
                        p.getDefaultPhysicalPartition().getLatestBaseIndex().getBalanceStat().isBalanced(),
                        "De-colocated t1 partitions must be skipped via continue TABLE");
            }
            Assertions.assertFalse(
                    t2.getPartitions().iterator().next()
                            .getDefaultPhysicalPartition().getLatestBaseIndex().getBalanceStat().isBalanced(),
                    "Second table t2 must still be checked after continue TABLE on t1");
        } finally {
            colocateIndex.removeTable(t1.getId(), t1, false);
            colocateIndex.removeTable(t2.getId(), t2, false);
        }
    }

    @Test
    public void testRevalidationSkipsDroppedPartitionAndContinuesToNextPartition() throws Exception {
        starRocksAssert.withDatabase("db_reval_part").useDatabase("db_reval_part")
                .withTable("CREATE TABLE db_reval_part.t_multi_part(k1 INT NOT NULL) DUPLICATE KEY(k1)\n" +
                        "PARTITION BY RANGE(k1) (\n" +
                        "  PARTITION p1 VALUES LESS THAN ('10'),\n" +
                        "  PARTITION p2 VALUES LESS THAN ('20')\n" +
                        ")\n" +
                        "DISTRIBUTED BY HASH(k1) BUCKETS 1\n" +
                        "PROPERTIES('replication_num' = '1', 'colocate_with' = 'grp_reval_part');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_reval_part");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_multi_part");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            colocateIndex.addBackendsPerBucketSeq(groupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L)));
            Config.tablet_checker_partition_batch_num = 1;

            List<Partition> orderedPartitions = Lists.newArrayList(
                    globalStateMgr.getLocalMetastore().getPartitionsIncludeRecycleBin(tbl));
            Partition firstPartition = orderedPartitions.get(0);
            Partition secondPartition = orderedPartitions.get(1);

            AtomicBoolean droppedFirst = new AtomicBoolean(false);
            new MockUp<YieldableLock>() {
                @Mock
                public void refresh(mockit.Invocation inv) {
                    inv.proceed();
                    if (droppedFirst.compareAndSet(false, true)) {
                        tbl.dropPartitionAndReserveTablet(firstPartition.getName());
                    }
                }
            };

            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                    groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(Status.UNSTABLE, result.status);
            Assertions.assertTrue(
                    firstPartition.getDefaultPhysicalPartition().getLatestBaseIndex().getBalanceStat().isBalanced(),
                    "Dropped first partition must be skipped via continue PARTITION without setting BalanceStat");
            Assertions.assertFalse(
                    secondPartition.getDefaultPhysicalPartition().getLatestBaseIndex().getBalanceStat().isBalanced(),
                    "Remaining second partition must still be checked after continue PARTITION");
        } finally {
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testAbortWhenInterruptedDuringBlockingAddTabletCtxToScheduler() throws Exception {
        starRocksAssert.withDatabase("db_reval_intr").useDatabase("db_reval_intr")
                .withTable("CREATE TABLE db_reval_intr.t_intr(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 2 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_reval_intr');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_reval_intr");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_intr");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            colocateIndex.addBackendsPerBucketSeq(groupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L), Lists.newArrayList(99999999L)));

            new MockUp<LocalTablet>() {
                @Mock
                public boolean readyToBeRepaired(TabletHealthStatus status, TabletSchedCtx.Priority priority) {
                    return true;
                }
            };

            AtomicInteger blockingAddCalls = new AtomicInteger(0);
            new MockUp<TabletScheduler>() {
                @Mock
                public Pair<Boolean, Long> blockingAddTabletCtxToScheduler(TabletSchedCtx ctx, boolean force,
                                                                           YieldableLock heldLock) {
                    blockingAddCalls.incrementAndGet();
                    Thread.currentThread().interrupt();
                    return Pair.create(false, 10L);
                }
            };

            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                    groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(Status.UNKNOWN, result.status,
                    "Interrupted blockingAddTabletCtxToScheduler must abort doMatchOneGroup with UNKNOWN status");
            Assertions.assertEquals(1, blockingAddCalls.get(),
                    "Must break TABLE immediately upon detecting thread interrupt");
        } finally {
            Thread.interrupted();
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testReplacedOrBucketMismatchedIndexReturnsUnknownInsteadOfStable() throws Exception {
        starRocksAssert.withDatabase("db_idx_mismatch").useDatabase("db_idx_mismatch")
                .withTable("CREATE TABLE db_idx_mismatch.t_mismatch(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 2 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_idx_mismatch');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_idx_mismatch");
        OlapTable tbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_mismatch");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId groupId = colocateIndex.getGroup(tbl.getId());

        try {
            colocateIndex.markGroupUnstable(groupId, false);
            // Set 1 bucket in backendBucketsSeq while the table index has 2 tablets (bucket mismatch)
            colocateIndex.addBackendsPerBucketSeq(groupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L)));

            ColocateMatchResult result = Deencapsulation.invoke(BALANCER, "doMatchOneGroup",
                    groupId, false, globalStateMgr, colocateIndex, globalStateMgr.getTabletScheduler());
            Assertions.assertEquals(Status.UNKNOWN, result.status,
                    "Unchecked index with bucket-size mismatch must return UNKNOWN, never STABLE");
        } finally {
            colocateIndex.removeTable(tbl.getId(), tbl, false);
        }
    }

    @Test
    public void testMatchGroupsIsolatesRuntimeExceptionPerGroup() throws Exception {
        starRocksAssert.withDatabase("db_group_isolation").useDatabase("db_group_isolation")
                .withTable("CREATE TABLE db_group_isolation.t_poison(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 1 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_poison');")
                .withTable("CREATE TABLE db_group_isolation.t_second(id INT NOT NULL) " +
                        "distributed by hash(`id`) buckets 1 " +
                        "properties('replication_num' = '1', 'colocate_with' = 'grp_second');");

        GlobalStateMgr globalStateMgr = GlobalStateMgr.getCurrentState();
        Database db = globalStateMgr.getLocalMetastore().getDb("db_group_isolation");
        OlapTable poisonTbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_poison");
        OlapTable secondTbl = (OlapTable) globalStateMgr.getLocalMetastore().getTable(db.getFullName(), "t_second");
        ColocateTableIndex colocateIndex = globalStateMgr.getColocateTableIndex();
        GroupId poisonGroupId = colocateIndex.getGroup(poisonTbl.getId());
        GroupId secondGroupId = colocateIndex.getGroup(secondTbl.getId());

        try {
            // Corrupt poisonGroupId's bucket sequence (2 replicas in bucket vs replicationNum = 1)
            // so Preconditions.checkState(bucketSeq.size() == replicationNum) throws IllegalStateException
            colocateIndex.addBackendsPerBucketSeq(poisonGroupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999998L, 99999999L)));

            // secondGroupId has a mismatched backend and starts marked stable
            colocateIndex.addBackendsPerBucketSeq(secondGroupId,
                    Lists.<List<Long>>newArrayList(Lists.newArrayList(99999999L)));
            colocateIndex.markGroupStable(secondGroupId, false);
            Assertions.assertFalse(colocateIndex.isGroupUnstable(secondGroupId));

            // Ensure poisonGroupId is visited before secondGroupId
            new MockUp<ColocateTableIndex>() {
                @Mock
                public Set<GroupId> getAllGroupIds() {
                    java.util.LinkedHashSet<GroupId> ordered = new java.util.LinkedHashSet<>();
                    ordered.add(poisonGroupId);
                    ordered.add(secondGroupId);
                    return ordered;
                }
            };

            Deencapsulation.invoke(BALANCER, "matchGroups");
            Assertions.assertTrue(colocateIndex.isGroupUnstable(secondGroupId),
                    "Exception in poisonGroupId must not prevent secondGroupId from being matched and marked unstable");
        } finally {
            colocateIndex.removeTable(poisonTbl.getId(), poisonTbl, false);
            colocateIndex.removeTable(secondTbl.getId(), secondTbl, false);
        }
    }
}
