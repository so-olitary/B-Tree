﻿#include "stdafx.h"
#include "mydb_block.h"
#include "mydb_techb.h"

//-------------------------------------------------------------------------------------------------------------
/* INTERFACE */
sDB* mydb_create (IN const char *file, IN const sDBC *conf);
int  mydb_close  (IN sDB *db);
int  mydb_delete (IN sDB *db, IN const sDBT *key);
int  mydb_insert (IN sDB *db, IN const sDBT *key, IN  const sDBT *data);
int  mydb_select (IN sDB *db, IN const sDBT *key, OUT       sDBT *data);
int  mydb_sync   (IN sDB *db);
//-------------------------------------------------------------------------------------------------------------
int  db_header_write (IN sDB *db)
{
  //-----------------------------------------
  if ( lseek (db->hfile_, 0, SEEK_SET)
      || write (db->hfile_, &db->head_, sizeof (sDBFH)) != sizeof (sDBFH) )
  {
    fprintf (stderr, "DB Header: %s\n", strerror (errno));
    return 1;
  }
  //-----------------------------------------
  return 0;
}
//-------------------------------------------------------------------------------------------------------------
/*  -1 = (this_key > that_key); 0 = (this_key == that_key); 1 = (this_key < that_key)  */
int  key_compare (IN const sDBT *this_key, IN const sDBT *that_key)
{ // if ( this_key->size != that_key->size )
  //  return (this_key->size > that_key->size) ? 1 : -1;
  int res = memcmp (this_key->data, that_key->data, // this_key->size);
                    (this_key->size <= that_key->size) ?
                    this_key->size : that_key->size);
  return res;
}
//-------------------------------------------------------------------------------------------------------------
sDB* dbcreate (const char *file, const sDBC *conf)
{
  return mydb_create (file, conf);
}
sDB* dbopen   (const char *file, const sDBC *conf)
{
  return mydb_create (file, conf);
}
//-------------------------------------------------------------------------------------------------------------
/* Open DB, if it exists, otherwise create DB */
sDB* mydb_create (IN const char *file, IN const sDBC *conf)
{
  const char *error_prefix = "mydb creation";
  bool  fail = false;
  bool  file_exists = false;
  sDB  *db = NULL;
  //-----------------------------------------
  if ( conf->page_size <= sizeof (sDBFH) )
  {
    fail = true;
    mydb_errno = MYDB_ERR_FPARAM;
    fprintf (stderr, "%s%s page_size <= page_header = %d.\n",
             error_prefix, strmyerror (mydb_errno), sizeof (sDBFH));
    goto MYDB_FREE;
  }

  db = (sDB*) calloc (1U, sizeof (sDB));
  if ( !db )
  {
    fail = true;
    fprintf (stderr, "%s%s\n", error_prefix, strerror (errno));
    goto MYDB_FREE;
  }
  //-----------------------------------------
  db->close  = &mydb_close;
  db->delete = &mydb_delete;
  db->select = &mydb_select;
  db->insert = &mydb_insert;
  db->sync   = &mydb_sync;
  //-----------------------------------------
  /* Check for db file existence */
  if ( access (file, F_OK | R_OK | W_OK) != -1 )
  {
    file_exists = true;
  }
  else
  {
    mydb_errno = MYDB_ERR_FNEXST;
    printf ("%s%s\n", error_prefix, strmyerror (mydb_errno));
  }
  //-----------------------------------------
  db->hfile_ = open (file, O_CREAT | O_RDWR | O_BINARY, S_IREAD | S_IWRITE);
  if ( db->hfile_ == -1 )
  {
    fail = true;
    fprintf (stderr, "%s%s\n", error_prefix, strerror (errno));
    goto MYDB_FREE;
  }
  //-----------------------------------------
  if ( file_exists ) /* open existing file */
  {
    if ( read (db->hfile_, &db->head_, sizeof (sDBFH)) != sizeof (sDBFH) )
    {
      fail = true;
      fprintf (stderr, "%s%s\n", error_prefix, strerror (errno));
      goto MYDB_FREE;
    } // end if read

    if ( db->head_.db_size_   != conf->db_size
      || db->head_.page_size_ != conf->page_size )
    {
      fail = true;
      mydb_errno = MYDB_ERR_FPARAM;
      fprintf (stderr, "%s%s File config don't equal actual.\n",
               error_prefix, strmyerror (mydb_errno));
      goto MYDB_FREE;


    } // end if conf
  } // end if file_exists
  else /* if !file_exists - create a new file */
  {
    uchar_t  c = 0;
    long  mydb_file_size = 0L;

    db->head_.db_size_     =  conf->db_size;
    db->head_.page_size_   =  conf->page_size;
    db->head_.block_count_ = (conf->db_size - sizeof (sDBFH)) / conf->page_size;
    db->head_.nodes_count_ = (0U);
    db->head_.techb_count_ = (db->head_.block_count_ - 1U)  / MYDB_BITSINBYTE + 1U; /* round to large integer */
    db->head_.offset2root_ = MYDB_OFFSET2NEW; /* first non-tech.block */
    //-----------------------------------------
    mydb_file_size = (db->head_.block_count_) * conf->page_size - 1U;

    /* (File-Header + Memory-Blocks) allocate*/
    if ( write (db->hfile_, &db->head_, sizeof (sDBFH)) != sizeof (sDBFH)
      || lseek (db->hfile_, mydb_file_size, SEEK_CUR) != mydb_file_size + sizeof (sDBFH)
      || write (db->hfile_, &c,  sizeof (uchar_t)) != sizeof (uchar_t))
    {
      fail = true;
      fprintf (stderr, "%s%s\n", error_prefix, strerror (errno));
      goto MYDB_FREE;
    }

    /* Seek blocks set, after header */
    lseek (db->hfile_, sizeof (sDBFH), SEEK_SET);
  } // end else file_exists
  //-----------------------------------------
  db->techb_arr_ = calloc (db->head_.techb_count_, sizeof (sTechB));
  if ( !db->techb_arr_ )
  {
    fail = true;
    fprintf (stderr, "%s%s\n", error_prefix, strerror (errno));
    goto MYDB_FREE;
  }

  for ( uint_t i = 0U; i < db->head_.techb_count_; ++i )
  {
    if ( !techb_create (db, (uchar_t*) &db->techb_arr_[i], i) )
    {
      fail = true;
      goto MYDB_FREE;
    }
  }
  //-----------------------------------------
  db->root_ = block_create (db, db->head_.offset2root_);
  if( !db->root_ )
  {
    fail = true;
    goto MYDB_FREE;
  }
  
  if ( !file_exists )
  {
    (*block_type (db->root_)) = Leaf;
    db->head_.offset2root_ = db->root_->offset_;
    if ( db_header_write (db) )
      fail = true;
  }
  //-----------------------------------------
MYDB_FREE:
  if ( fail )
  {
    db->close (db);
    db = NULL;
  }
  //-----------------------------------------
  return db;
}
int  mydb_close  (IN sDB *db)
{
  if ( db )
  {
    //-----------------------------------------
    db_header_write (db);
    //-----------------------------------------
    techb_sync (db);
    for ( uint_t i = 0U; i < db->head_.techb_count_; ++i )
      techb_destroy (&db->techb_arr_[i]);
    free (db->techb_arr_);
    db->techb_arr_ = NULL;
    //-----------------------------------------
    block_destroy (db->root_);
    //-----------------------------------------
    close (db->hfile_);
    free  (db);
    //-----------------------------------------
  }
  return 0;
}
int  mydb_delete (IN sDB *db, IN const sDBT *key)
{
  return block_deep_delete (db->root_, key)
         || db_header_write (db);
}
int  mydb_insert (IN sDB *db, IN const sDBT *key, IN  const sDBT *data)
{
  int  result = 0;
  sBlock *rb = db->root_;
  //-----------------------------------------
  if ( block_isfull (rb) )
  {
    sBlock *new_root, *new_block;
    /* alloc new free blocks */
    new_root  = block_create (db, MYDB_OFFSET2NEW);
    new_block = block_create (db, MYDB_OFFSET2NEW);
    if ( !new_root || !new_block )
      return -1;
    
    // block type of rb was Pass or Leaf
    (*block_type (new_block)) = (*block_type (rb));

    (*block_type (new_root)) = Pass; // Root;
    (*block_nkvs (new_root)) = 0;
    (*block_rptr (new_root, NULL)) = rb->offset_;
    
    db->root_ = new_root;
    db->head_.offset2root_ = new_root->offset_;
    
    if ( block_split_child (new_root, rb, new_block)
      || block_add_nonfull (new_root, key, data) )
      result = -1;
    //-----------------------------------------
    block_destroy (rb);
    block_destroy (new_block);
    //-----------------------------------------
  }
  else
  {
    result = block_add_nonfull (rb, key, data);
  }
  //-----------------------------------------
  return result || db_header_write (db);
}
int  mydb_select (IN sDB *db, IN const sDBT *key, OUT       sDBT *data)
{ return block_select_data (db->root_, key, data); }
int  mydb_sync   (IN sDB *db)
{
  /* cache */
  return 0;
}
//-------------------------------------------------------------------------------------------------------------
